const {
  produceRtp,
  consumeRtp,
  saveAudio,
  createSrtpParameters,
  createRtpParameters,
  createSDP,
} = require("../src/index.ts");

const { exec } = require("child_process");
const fs = require("fs");
const path = require("path");
const { randomInt } = require("crypto");

const RTP_PORT = 9004;

// This should match the test wav file
const encodeSampleRate = 24000;

// Decode at a different sample rate.
const decodeSampleRate = 16000;

const decodedBytesToMs = (bytes) => (bytes / (decodeSampleRate * 2)) * 1000;

async function runProducer({
  rtpParameters,
  srtpParameters,
  signal,
  queueDepth,
}) {
  let resolveDrain;
  let drainCount = 0;

  const producer = produceRtp({
    ipAddress: "127.0.0.1",
    rtpPort: RTP_PORT,
    rtcpPort: RTP_PORT + 1,
    rtpParameters,
    srtpParameters,
    signal,
    onError: (error) => {
      console.log("producer error callback", error);
    },
    onDrain: () => {
      drainCount++;
      if (resolveDrain) {
        resolveDrain();
        resolveDrain = undefined;
      }
    },
    sampleRate: encodeSampleRate,
    opus: {
      bitrate: null,
      packetLossPercent: 10,
    },
    queueDepth,
  });

  // LJ025-0076.wav from https://keithito.com/LJ-Speech-Dataset/
  // Note: encoder expects 24000Hz mono 16-bit PCM. If the source file is a
  // different sample rate it should be resampled beforehand.
  //const sourceAudio = path.join(__dirname, "LJ025-0076.wav");
  const sourceAudio = path.join(__dirname, "LJ025-0076_24k_mono.wav");
  const fileBuffer = fs.readFileSync(sourceAudio);
  const pcmData = fileBuffer.subarray(44); // skip WAV header

  // The audio_encode_thread has an accumulator that buffers PCM data until
  // it's big enough to fill an opus frame. We use random input buffers to exercise
  // this logic.
  const chunkRange = [10, 960];
  let chunkSize = randomInt(...chunkRange);

  for (let offset = 0; offset < pcmData.length; ) {
    const chunk = pcmData.subarray(
      offset,
      Math.min(offset + chunkSize, pcmData.length),
    );

    // NOTE: I've noticed that adding a console.log line will slow down tests enough
    // to trigger the jest timeout.
    console.log(`Writing ${chunk.length} bytes`);

    offset += chunkSize;
    chunkSize = randomInt(...chunkRange);

    // When write() returns false, the data was dropped. Wait for drain and retry.
    while (!producer.write(chunk)) {
      await new Promise((resolve) => {
        resolveDrain = resolve;
      });
    }
  }
  producer.end();

  return { done: producer.done, drainCount };
}

it("aborts a producer thread", async () => {
  const rtpParameters = createRtpParameters();
  const srtpParameters = createSrtpParameters();

  const abortController = new AbortController();

  const { done } = await runProducer({
    rtpParameters,
    srtpParameters,
    signal: abortController.signal,
  });

  abortController.abort();

  // This should abort right away
  await done();
});

it(
  "starts an audio encode/decode thread",
  async () => {
    const rtpParameters = createRtpParameters();
    const srtpParameters = createSrtpParameters();

    const sdp = createSDP({
      subject: "Unit Test",
      rtpParameters,
      originIpAddress: "127.0.0.1",
      destinationIpAddress: "127.0.0.1",
      rtpPort: RTP_PORT,
      rtcpPort: RTP_PORT + 1,
      language: "en",
      srtpParameters,
    });

    let buffersReceived = 0;
    // Play with: ffplay -f s16le -ar 16000 -ch_layout mono tests/decoded_output.pcm
    const pcmPath = path.join(__dirname, "decoded_output.pcm");
    const pcmStream = fs.createWriteStream(pcmPath);
    function onAudioData({ buffer, pts }) {
      expect(buffer.byteLength).toBeGreaterThan(0);
      pcmStream.write(buffer);
      buffersReceived++;
    }

    const abortController = new AbortController();

    const { done: consumerDone } = consumeRtp({
      sdp,
      onAudioData,
      onError: (error) => {
        console.log("consumer error", error);
      },
      sampleRate: decodeSampleRate,
      signal: abortController.signal,
    });

    // Start the producer after the decoder is listening.
    // Use an absurdly small queue to test backpressure handling.
    const { done: producerDone, drainCount } = await runProducer({
      rtpParameters,
      srtpParameters,
      signal: abortController.signal,
      queueDepth: 4,
    });

    // With queueDepth: 4 and a full WAV file, drain should have fired many times
    expect(drainCount).toBeGreaterThan(0);

    // Wait for the producer to finish streaming audio. Should take less than 10 seconds
    await producerDone();

    // Signal for the consumer to shutdown
    abortController.abort();

    await consumerDone();

    pcmStream.end();

    // This will usually be 420
    expect(buffersReceived).toBeGreaterThan(410);
  },
  10 * 1000,
);

it(
  "caps packet-loss concealment and flags the discontinuity after a long gap",
  async () => {
    const GAP_RTP_PORT = 9020;
    const GAP_MS = 5000;
    const CONCEALMENT_CAP_MS = 3000;
    const SEGMENT_MS = 1000;

    const rtpParameters = createRtpParameters();
    const srtpParameters = createSrtpParameters();

    const sdp = createSDP({
      subject: "Gap Test",
      rtpParameters,
      originIpAddress: "127.0.0.1",
      destinationIpAddress: "127.0.0.1",
      rtpPort: GAP_RTP_PORT,
      rtcpPort: GAP_RTP_PORT + 1,
      srtpParameters,
    });

    const abortController = new AbortController();

    let decodedBytes = 0;
    let concealedBytes = 0;
    const discontinuities = [];
    const consumer = consumeRtp({
      sdp,
      sampleRate: decodeSampleRate,
      signal: abortController.signal,
      onAudioData: ({ buffer, pts, discontinuity, concealed }) => {
        decodedBytes += buffer.byteLength;
        if (concealed) {
          concealedBytes += buffer.byteLength;
        }
        if (discontinuity) {
          discontinuities.push(pts);
        }
      },
      onError: (error) => {
        console.log("consumer error", error);
      },
    });

    // No keep-alives: they would carry wall-clock timestamps through the gap
    // and turn the single long gap into a train of short ones.
    const producer = produceRtp({
      ipAddress: "127.0.0.1",
      rtpPort: GAP_RTP_PORT,
      rtcpPort: GAP_RTP_PORT + 1,
      rtpParameters,
      srtpParameters,
      signal: abortController.signal,
      keepAlive: false,
      sampleRate: encodeSampleRate,
      onError: (error) => {
        console.log("producer error", error);
      },
    });

    const pcmData = fs.readFileSync(
      path.join(__dirname, "LJ025-0076_24k_mono.wav"),
    ).subarray(44);
    const segment = pcmData.subarray(0, (encodeSampleRate * SEGMENT_MS * 2) / 1000);
    const writeSegment = () => {
      const chunkSize = 960;
      for (let offset = 0; offset < segment.length; offset += chunkSize) {
        producer.write(segment.subarray(offset, offset + chunkSize));
      }
    };

    writeSegment();
    // endSegment() rebases the next packet to wall-clock time, so the idle
    // period becomes an RTP timestamp gap of GAP_MS.
    producer.endSegment();
    await new Promise((resolve) => setTimeout(resolve, GAP_MS));
    writeSegment();
    producer.end();
    await producer.done();

    await new Promise((resolve) => setTimeout(resolve, 500));
    abortController.abort();
    await consumer.done();

    // Two segments of real audio plus at most the concealment cap, not the
    // whole gap.
    const decodedMs = decodedBytesToMs(decodedBytes);
    expect(decodedMs).toBeGreaterThan(2 * SEGMENT_MS + CONCEALMENT_CAP_MS - 500);
    expect(decodedMs).toBeLessThan(2 * SEGMENT_MS + CONCEALMENT_CAP_MS + 500);
    expect(discontinuities).toHaveLength(1);
    // Everything synthesized for the gap, and nothing else, is marked.
    const concealedMs = decodedBytesToMs(concealedBytes);
    expect(concealedMs).toBeGreaterThan(CONCEALMENT_CAP_MS - 100);
    expect(concealedMs).toBeLessThan(CONCEALMENT_CAP_MS + 100);
  },
  20 * 1000,
);

it(
  "marks the frames concealed during DTX silence, not the comfort-noise refreshes",
  async () => {
    const DTX_RTP_PORT = 9030;
    const SPEECH_MS = 1000;
    const SILENCE_MS = 2000;

    const rtpParameters = createRtpParameters({ enableDtx: true });
    const srtpParameters = createSrtpParameters();

    const sdp = createSDP({
      subject: "DTX Concealment Test",
      rtpParameters,
      originIpAddress: "127.0.0.1",
      destinationIpAddress: "127.0.0.1",
      rtpPort: DTX_RTP_PORT,
      rtcpPort: DTX_RTP_PORT + 1,
      srtpParameters,
    });

    const abortController = new AbortController();

    const frames = [];
    const consumer = consumeRtp({
      sdp,
      sampleRate: decodeSampleRate,
      signal: abortController.signal,
      onAudioData: ({ buffer, pts, concealed }) => {
        frames.push({ pts, concealed, bytes: buffer.byteLength });
      },
      onError: (error) => {
        console.log("consumer error", error);
      },
    });

    let resolveDrain;
    // No keep-alives: the test writes faster than real time, so only the
    // encoder's DTX decides what goes on the wire.
    const producer = produceRtp({
      ipAddress: "127.0.0.1",
      rtpPort: DTX_RTP_PORT,
      rtcpPort: DTX_RTP_PORT + 1,
      rtpParameters,
      srtpParameters,
      signal: abortController.signal,
      keepAlive: false,
      sampleRate: encodeSampleRate,
      onDrain: () => {
        if (resolveDrain) {
          resolveDrain();
          resolveDrain = undefined;
        }
      },
      onError: (error) => {
        console.log("producer error", error);
      },
    });

    const pcmData = fs.readFileSync(
      path.join(__dirname, "LJ025-0076_24k_mono.wav"),
    ).subarray(44);
    const msToBytes = (ms) => (encodeSampleRate * ms * 2) / 1000;
    const speech = pcmData.subarray(0, msToBytes(SPEECH_MS));
    const silence = Buffer.alloc(msToBytes(SILENCE_MS));
    const writeAll = async (pcm) => {
      const chunkSize = 960;
      for (let offset = 0; offset < pcm.length; offset += chunkSize) {
        const chunk = pcm.subarray(offset, offset + chunkSize);
        while (!producer.write(chunk)) {
          await new Promise((resolve) => {
            resolveDrain = resolve;
          });
        }
      }
    };

    await writeAll(speech);
    await writeAll(silence);
    await writeAll(speech);
    producer.end();
    await producer.done();

    await new Promise((resolve) => setTimeout(resolve, 500));
    abortController.abort();
    await consumer.done();

    const sumMs = (list) => decodedBytesToMs(list.reduce((sum, f) => sum + f.bytes, 0));
    const totalMs = sumMs(frames);
    const concealedMs = sumMs(frames.filter((f) => f.concealed));
    // The stream stays continuous, and most of the silence was never sent:
    // libopus keeps sending for ~200ms before DTX starts, then sends one
    // refresh frame per ~400ms.
    expect(totalMs).toBeGreaterThan(2 * SPEECH_MS + SILENCE_MS - 300);
    expect(totalMs).toBeLessThan(2 * SPEECH_MS + SILENCE_MS + 300);
    expect(concealedMs).toBeGreaterThan(SILENCE_MS / 2);
    expect(concealedMs).toBeLessThan(SILENCE_MS + 500);

    // The refreshes inside the silence arrive as real frames.
    const basePts = frames[0].pts;
    const ptsToMs = (pts) => ((pts - basePts) * 1000) / 48000;
    const realInSilence = frames.filter((f) => {
      const atMs = ptsToMs(f.pts);
      return !f.concealed && atMs > SPEECH_MS + 300 && atMs < SPEECH_MS + SILENCE_MS - 50;
    });
    expect(realInSilence.length).toBeGreaterThanOrEqual(3);
  },
  20 * 1000,
);

it(
  "saves RTP audio to a webm file via saveAudio",
  async () => {
    const SAVE_RTP_PORT = 9010;
    const rtpParameters = createRtpParameters();
    const srtpParameters = createSrtpParameters();

    const sdp = createSDP({
      subject: "Save Audio Test",
      rtpParameters,
      originIpAddress: "127.0.0.1",
      destinationIpAddress: "127.0.0.1",
      rtpPort: SAVE_RTP_PORT,
      rtcpPort: SAVE_RTP_PORT + 1,
      language: "en",
      srtpParameters,
    });

    const outputPath = path.join(__dirname, "save_audio_output.webm");

    // Clean up any previous output
    if (fs.existsSync(outputPath)) {
      fs.unlinkSync(outputPath);
    }

    const abortController = new AbortController();

    const saver = saveAudio({
      sdp,
      outputPath,
      signal: abortController.signal,
      onError: (error) => {
        console.log("saveAudio error", error);
      },
    });

    // Start the producer to stream audio to the same port
    const producer = produceRtp({
      ipAddress: "127.0.0.1",
      rtpPort: SAVE_RTP_PORT,
      rtcpPort: SAVE_RTP_PORT + 1,
      rtpParameters,
      srtpParameters,
      signal: abortController.signal,
      onError: (error) => {
        console.log("producer error", error);
      },
      sampleRate: encodeSampleRate,
    });

    const sourceAudio = path.join(__dirname, "LJ025-0076_24k_mono.wav");
    const fileBuffer = fs.readFileSync(sourceAudio);
    const pcmData = fileBuffer.subarray(44);

    const chunkSize = 960;
    for (let offset = 0; offset < pcmData.length; offset += chunkSize) {
      const chunk = pcmData.subarray(
        offset,
        Math.min(offset + chunkSize, pcmData.length),
      );
      producer.write(chunk);
    }
    producer.end();

    // Wait for the producer to finish streaming
    await producer.done();

    // Give saveAudio a moment to receive the last packets
    await new Promise((resolve) => setTimeout(resolve, 500));

    // Signal saveAudio to stop and flush the file
    abortController.abort();
    await saver.done();

    // Verify the file exists and has content
    expect(fs.existsSync(outputPath)).toBe(true);
    const stats = fs.statSync(outputPath);
    expect(stats.size).toBeGreaterThan(1000);

    // Verify duration with ffprobe
    const duration = await new Promise((resolve, reject) => {
      exec(
        `ffprobe -v quiet -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "${outputPath}"`,
        (error, stdout) => {
          if (error) {
            reject(error);
          } else {
            resolve(parseFloat(stdout.trim()));
          }
        },
      );
    });

    console.log(`saveAudio output duration: ${duration}s`);

    // The source WAV is ~8.4s. Allow some tolerance for RTP timing.
    expect(duration).toBeGreaterThan(5);
    expect(duration).toBeLessThan(15);
  },
  15 * 1000,
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
