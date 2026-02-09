const native = require("../build/Release/worker.node");
const { randomBytes, getRandomValues } = require("crypto");

type ProduceOptions = {
  ipAddress: string;
  rtpPort: number;
  rtcpPort: number;
  enableSrtp: boolean;
  onError: (error: Error) => void;
  // sample rate of the pcm audio data that will be passed into the sendAudioData function
  sampleRate: number;
  opus?: {
    bitrate?: number | null;
    enableFec?: boolean;
    packetLossPercent?: number;
  };
};

type ProduceReturn = {
  sendAudioData: (data: Buffer) => void;
  flush: () => void;
  setBitrate: (bitrate: number | null) => void;
  setEnableFec: (enableFec: boolean) => void;
  setPacketLossPercent: (percent: number) => void;
  shutdown: (options?: { immediate?: false }) => Promise<void>;
};

type ConsumeOptions = {
  sdp: string;
  onAudioData: (data: Buffer) => void;
  onError: (error: Error) => void;

  // Sample rate that the audio data will be decoded to.
  // Must be one of 8000, 12000, 16000, 24000, 48000
  sampleRate: number;
};

type ConsumeReturn = {
  shutdown: () => Promise<void>;
};

// TODO: Expose error codes and messages
class AudioRtpError extends Error {}

export function produceRtp(options: ProduceOptions): ProduceReturn {
  const rtpUrl = `rtp://127.0.0.1:${options.rtpPort}`;
  const cname = randomBytes(8).toString("hex");
  const list = new Int32Array(1);
  getRandomValues(list);
  const ssrc = Math.abs(list[0]);

  // TODO: I don't like hardcoding these
  const payloadType = "97";

  const abortController = new AbortController();

  const { promise, external } = native.startAudioEncodeThread(
    abortController.signal,
    {
      rtpUrl,
      ssrc: String(ssrc),
      payloadType: String(payloadType),
      cname,
      sampleRate: options.sampleRate,
      bitrate: options.opus?.bitrate ?? 0,
      enableFec: options.opus?.enableFec ?? false,
      packetLossPercent: options.opus?.packetLossPercent ?? 0,
    },
  );

  promise.catch((error: any) => {
    options.onError(error);
  });

  function shutdown(options: { immediate?: boolean } = {}) {
    if (options.immediate) {
      native.clearMessageQueue(external);
    }

    abortController.abort();
    return promise;
  }

  function sendAudioData(buffer: Buffer) {
    native.postPcmToEncoder(external, buffer);
  }

  function setBitrate(bitrate: number | null) {
    native.postSetBitrate(external, bitrate ?? 0);
  }

  function setEnableFec(enableFec: boolean) {
    native.postSetEnableFec(external, enableFec);
  }

  function setPacketLossPercent(percent: number) {
    native.postSetPacketLossPercent(external, percent);
  }

  function flush() {
    native.postFlushEncoder(external);
  }

  return {
    shutdown,
    sendAudioData,
    flush,
    setBitrate,
    setEnableFec,
    setPacketLossPercent,
  };
}

export function consumeRtp(options: ConsumeOptions): ConsumeReturn {
  const abortController = new AbortController();

  const { promise } = native.startAudioDecodeThread(
    dataUrl(options.sdp),
    options.onAudioData,
    abortController.signal,
    {
      sampleRate: options.sampleRate,
      channels: 1,
    },
  );

  promise.catch((error: any) => {
    options.onError(error);
  });

  function shutdown() {
    abortController.abort();
    return promise;
  }

  return { shutdown };
}

function dataUrl(input: string) {
  return "data:application/sdp;base64," + Buffer.from(input).toString("base64");
}
