const socket = require('socket.io-client')('http://localhost:5555')

socket.on('connect', () => { 
  console.log('connected');
  socket.emit('audio', { frequency: 1, unit: 0 });
})

socket.on('audio', data => {
  console.log(data);
});
