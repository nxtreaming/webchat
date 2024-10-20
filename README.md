# webchat

Text and Audio chat based on websocket.  

## Install 3rd package

git clone https://github.com/warmcat/libwebsockets.git  
cd libswebsockets  
git checkout v4.3-stable  
cmake .  
make && make install  
  
ldconfig  

## Compile  
git clone https://github.com/nxtreaming/webchat.git  
cd webchat   
make
./websocket_server  
