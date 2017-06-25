#include "objworker.h"
#include "objserver.h"
#include "log.h"
#include <ctime>
#include <string>
#include <iomanip>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <boost/algorithm/string.hpp>
#include <vector>
#include <sys/stat.h>
#include <fstream>
#include <sys/sendfile.h>
#include <iosfwd>
#include <netinet/tcp.h>

#define BUFSIZE 1024 * 1024
#define min(a,b) (a<b?a:b)
using namespace std;

ObjWorker::ObjWorker(ObjServer &objserver, int socket)
  : objserver(objserver)
  , socket(socket) {
  fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
  int yes = 1;
  if (setsockopt(socket, SOL_TCP/*IPPROTO_TCP*/, TCP_NODELAY, &yes, sizeof(int)))
    LOG_ERROR << "error: unable to set socket option";
  int tcp_send_buf = BUFSIZE * 10;
  if (setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &tcp_send_buf, sizeof(tcp_send_buf)) < 0)
    LOG_ERROR << "Error setsockopt";
}

void ObjWorker::exit()
{
  close(socket);
  LOG_INFO << "client lost";
  pthread_exit(nullptr);
}

int ObjWorker::reply(int socket, const char* msg, int size, int retry, bool silent) {
  if (!silent)
    LOG_DEBUG << "Replying: " << msg;
  int ret, i;
  int sent = 0;
  //int cork = 0;
  //setsockopt(socket, SOL_TCP, TCP_CORK, &cork, sizeof(cork));
  int remaining = size;
  bool keep_retry = retry < 0;
  for(i = 0; keep_retry || i < retry; i++) {
    ret = write(socket, msg + sent, remaining);
    if(ret > 0) {
      //LOG_DEBUG << "write " << ret << " bytes";
      sent += ret;
      remaining -= ret;
      if (remaining == 0)
        break;
    } else {
      //LOG_DEBUG << "Can't write socket, ret = " << ret << " error = " << std::strerror(errno);
    }
  }
  if (retry > 0 && i >= retry)
    LOG_ERROR << "Retried sending for " << i << " times";
  //cork = 1;
  //setsockopt(socket, SOL_TCP, TCP_CORK, &cork, sizeof(cork));
  return sent;
}

void ObjWorker::handle_get(vector<string> parts){
  string fn("/dev/shm/" + parts[1]);

  struct stat fileStat;
  if(stat(fn.c_str() ,&fileStat) != 0) {
    LOG_ERROR << "Failed to open file " << fn << ", err " << strerror(errno);
    string response = "get_fail|" + parts[1] + ";";
    reply(socket, response.c_str(), response.size());
  } else {
    LOG_DEBUG << "Found file " << fn;
    string response("get_success|" + parts[1] + "|" + to_string(fileStat.st_size) + ";");
    reply(socket, response.c_str(), response.size());
#if USESENDFILE
    int shm_file = open(fn.c_str(), O_RDONLY);
    if (shm_file < 0)
      LOG_ERROR << "failed to open file, errno " << strerror(errno);
    int sent;
    uint64_t total_sent = 0;
    while (total_sent < fileStat.st_size) {
      sent = sendfile(socket, shm_file, NULL, fileStat.st_size - total_sent);
      if(sent >= 0) {
        total_sent += sent;
        LOG_TRACE << "sent " << sent << " total_sent " << total_sent;
      }
      else if (errno == EINTR || errno == EAGAIN) {
        //LOG_ERROR << "send error, sent = " << sent << " errno " << strerror(errno);
        continue;
      } else {
        LOG_ERROR << "Sent = " << sent << " errno = " << strerror(errno);
        break;
      }
    }
    if (total_sent != fileStat.st_size)
      LOG_ERROR << "sendfile fail, total_sent = " << total_sent << " fsize " << fileStat.st_size;
    LOG_DEBUG << "Sent " << total_sent << " bytes to client";
    close(shm_file);
#else
    //char* shm = mmap(0, fileStat.st_size, PROT_READ, MAP_SHARED, shm_fd, 0);
    ifstream shm_file(fn, ios::binary);
    char readBuf[BUFSIZE];
    uint64_t total_read = 0;
    size_t actual_read;
    while(!shm_file.eof()) {
      actual_read = shm_file.read(readBuf, BUFSIZE).gcount();
      total_read += actual_read;
      //LOG_DEBUG << "sending " << actual_read << " bytes";
      int actual_sent = reply(socket, readBuf, actual_read, -1, true);
      if (actual_sent != actual_read)
        LOG_ERROR << "Error: Actual sent = " << actual_sent << " Actual read = " << actual_read;
      LOG_TRACE << "Read " << actual_read << " Sent " << actual_sent << " Total Read " << total_read;
      if(total_read == fileStat.st_size)
        break;//needed with st_size is a multiple of BUFSIZE
    }
    if(total_read != fileStat.st_size)
      LOG_ERROR << "total_read != fileStat.st_size " << total_read << " " << fileStat.st_size;
    //close(shm_fd);
    shm_file.close();
    LOG_DEBUG << "Finished sending " << fn << " totalbytes: " << total_read; 
#endif
  } 
}


void ObjWorker::handle_put(vector<string> parts, char* remaining_start, int remaining_size){
  string fn("/dev/shm/" + parts[1]);
  char * end;
  uint64_t fsize = strtoull(parts[2].c_str(), &end, 10);
  int real_remaining = min(remaining_size, fsize);
 
  ofstream shm_file(fn, ios::binary);
  if(!shm_file.is_open()) {
    LOG_ERROR << "Failed to open file " << fn << ", err " << strerror(errno);
    return;
  } else {
    LOG_DEBUG << "Successfully open file " << fn << " Remaining bytes: " << remaining_size;
    if (remaining_size > 0) {
      shm_file.write(remaining_start, real_remaining);
    }
    char bodybuf[BUFSIZE];
    uint64_t rsize = real_remaining;
    int n;
    while(rsize < fsize) {
      n = read(socket, bodybuf, min(BUFSIZE, fsize - rsize));
      if (n <= 0) {
        LOG_ERROR << "Error reading, n = " << n << " errno " << strerror(errno);
        break;
      }
      shm_file.write(bodybuf, n);
      rsize += n;
      LOG_TRACE << "Received " << rsize << " bytes";
    }
    if (rsize != fsize) {
      LOG_ERROR << "header size: " << fsize << " received size: " << rsize;
    }
    shm_file.close();
    LOG_DEBUG << "Done receiving key " << fn << " size " << rsize;    
  }
  string response("put_success|" + parts[1] + ";");
  reply(socket, response.c_str(), response.size());

}

void ObjWorker::run() {
  char buffer[1024];

  for (;;) {
    int read_size;
    while ((read_size = read(socket, buffer, sizeof(buffer))) > 0) {
      for (int i = 0; i < read_size; ++i) {
        if (buffer[i] == ';') {
          buffer[i] = '\0';
          string msg(buffer);
          LOG_DEBUG << "Received msg: " << msg << " read_size:" << read_size;
          vector<string> parts;
          boost::split(parts, msg, boost::is_any_of("|"));
          if(parts[0] == "get") {
            handle_get(parts);
	  } else if (parts[0] == "put") {
            handle_put(parts, buffer + i + 1, read_size - (i + 1));
          }
          break;
        }
      }
    }
  }
}

void* ObjWorker::pthread_helper(void * worker) {
  ((ObjWorker*)worker)->run();
  return (void*) NULL;
}
