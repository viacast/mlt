const fs = require('fs');
const { exit } = require('process');
const server = require('http').createServer();
const io = require('socket.io')(server);

const AUDIO_FILE_PREFIX = process.argv[3] || '/dev/shm/melted_preview';
const PORT = process.argv[4] || 5555;

io.on('connection', client => {
  console.log("New connection.");
  let intervalHandle = 0;

  client.on('audio', data => { 
    frequency = data.frequency;
    unit = data.unit;

    if (unit === undefined || !frequency) {
      client.emit('audio', { message: 'unit or frequency missing' });
      return;
    }

    const file = `${AUDIO_FILE_PREFIX}.${unit}.vu`;

    intervalHandle = setInterval(() => {
      fs.readFile(file, (err, data) => {
        if (err) return;
        const channels = data.readUInt8();
        const audio = [];
        for (let i = 0; i < channels; ++i) {
          audio[i] = data.readUInt8(i+1);
        }

        client.emit('audio', { audio });
      })
    }, 1000/frequency);
  });

  client.on('disconnect', () => {
    console.log("Disconnected.");
    if (intervalHandle) {
      clearInterval(intervalHandle);
      intervalHandle = 0;
    }
  });
});

console.log(`Listening on port ${port}...`);
server.listen(port);
