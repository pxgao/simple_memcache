# simple_memcache
cmake .

make

require: pthread boost


server:

./objserver port


client:

python objclient.py serverip:port     
