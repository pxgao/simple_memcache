import socket

class ObjClient:
  def __init__(self, servers):
    self.servers = servers.split(",")
    self.conn = []
    for s in self.servers:
      ip = s.split(":")[0]
      port = int(s.split(":")[1])
      c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
      c.connect((ip, port))
      self.conn.append(c)
  
  def __del__(self):
    for c in self.conn:
      c.close()

  def get_server(self, key):
    return self.conn[hash(key)%len(self.conn)]

  def get(self, key):
    conn = self.get_server(key)
    conn.sendall("get|%s;" % key)
    value = ""
    while True:
      data = conn.recv(256)
      value += data
      if value.find(";"):
        received = value.split(";")
        header = received[0]
        size = int(header.split("|")[2])
        value = received[1]
        break
    while(len(value) < size):
      data = conn.recv(size - len(value))
      value += data
    return value

  def put(self, key, value):
    conn = self.get_server(key)
    conn.sendall("put|%s|%s;" % (key, len(value)))
    conn.sendall(value)
    conn.recv(12 + len(key) + 1)

c = ObjClient("10.0.3.221:1988")
c.put("aaa","asdfae")
v = c.get("aaa")
print v
