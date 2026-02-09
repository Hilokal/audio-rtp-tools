const { produceRtp, consumeRtp } = require("../src/index.ts");

const { exec } = require("child_process");
const { Buffer } = require("buffer");
const { getRandomValues } = require("crypto");
const fs = require("fs");
const path = require("path");

const RTP_PORT = 9004;

function buildSdp({ ssrc, payloadType }) {
  return [
    "v=0",
    `o=- ${ssrc} 0 IN IP4 127.0.0.1`,
    "s=Test",
    "c=IN IP4 127.0.0.1",
    "t=0 0",
    `m=audio ${RTP_PORT} RTP/AVP ${payloadType}`,
    `a=rtpmap:${payloadType} opus/48000/2`,
    "",
  ].join("\r\n");
}

function dataUrl(input) {
  return "data:application/sdp;base64," + Buffer.from(input).toString("base64");
}

async function waitFor(timeout) {
  return new Promise((resolve) => setTimeout(resolve, timeout));
}

function runProducer() {
  const list = new Int32Array(1);
  getRandomValues(list);
  const ssrc = Math.abs(list[0]);
  const payloadType = "97";

  const { sendAudioData, flush, shutdown } = produceRtp({
    ipAddress: "127.0.0.1",
    rtpPort: RTP_PORT,
    rtcpPort: RTP_PORT + 1,
    enableSrtp: false,
    onError: (error) => {
      console.log("producer error callback", error);
    },
    // sample rate of the pcm audio data that will be passed into the sendAudioData function
    sampleRate: 24000,
    opus: {
      bitrate: null,
      enableFec: true,
      packetLossPercent: 10,
    },
  });

  // LJ025-0076.wav from https://keithito.com/LJ-Speech-Dataset/
  // Note: encoder expects 24000Hz mono 16-bit PCM. If the source file is a
  // different sample rate it should be resampled beforehand.
  //const sourceAudio = path.join(__dirname, "LJ025-0076.wav");
  const sourceAudio = path.join(__dirname, "LJ025-0076_24k_mono.wav");
  const fileBuffer = fs.readFileSync(sourceAudio);
  const pcmData = fileBuffer.subarray(44); // skip WAV header

  // Send PCM data in chunks: 480 samples * 2 bytes = 960 bytes per 20ms frame
  const chunkSize = 960;
  let count = 0;
  for (let offset = 0; offset < pcmData.length; offset += chunkSize) {
    const chunk = pcmData.subarray(
      offset,
      Math.min(offset + chunkSize, pcmData.length),
    );
    sendAudioData(chunk);
  }
  flush();

  return { shutdown, ssrc, payloadType };
}

it(
  "starts an audio encode/decode thread",
  async () => {
    const sdp = buildSdp({ ssrc: 0, payloadType: "97" });

    let buffersReceived = 0;
    function onAudioData({ buffer, pts }) {
      //console.log("got packet", pts);
      expect(buffer.byteLength).toBeGreaterThan(0);
      buffersReceived++;
    }

    const { shutdown: shutdownConsumer } = consumeRtp({
      sdp,
      onAudioData,
      onError: (error) => {
        console.log("consumer error", error);
      },
      sampleRate: 24000,
    });

    // Start the producer after the decoder is listening
    const { promise: producerPromise, shutdown: shutdownProducer } =
      runProducer();

    // Wait for the producer to finish streaming audio. Should take less than 10 seconds
    // TODO: Implement this
    await shutdownProducer({ immediate: false });

    await shutdownConsumer();
  },
  10 * 1000,
);

afterAll(() => {
  return checkForMemoryLeaks();
});

async function checkForMemoryLeaks() {
  return new Promise((resolve, reject) => {
    exec(`/usr/bin/leaks ${process.pid}`, (error, stdout, stderr) => {
      console.log(stdout);
      if (error) {
        reject(error);
      } else {
        resolve(stdout);
      }
    });
  });
}
