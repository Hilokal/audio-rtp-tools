const native = require("../build/Release/worker.node");
const { randomBytes } = require("crypto");

export type SrtpParameters = {
  cryptoSuite: string;
  keyBase64: string;
};

type ProduceOptions = {
  ipAddress: string;
  rtpPort: number;
  rtcpPort: number;
  srtpParameters?: SrtpParameters;
  onError: (error: Error) => void;
  // sample rate of the pcm audio data that will be passed into the sendAudioData function
  sampleRate: number;
  ssrc: number;
  payloadType: number;
  cname: string;
  opus?: {
    bitrate?: number | null;
    enableFec?: boolean;
    packetLossPercent?: number;
  };
};

type ProduceReturn = {
  sendAudioData: (data: Buffer) => boolean;
  flush: () => void;
  setBitrate: (bitrate: number | null) => void;
  setEnableFec: (enableFec: boolean) => void;
  setPacketLossPercent: (percent: number) => void;
  shutdown: (options?: { immediate?: false }) => Promise<void>;
};

type ConsumeOptions = {
  sdp: string;
  onAudioData: (data: { buffer: Buffer; pts: number | null }) => void;
  onError: (error: Error) => void;

  // Sample rate that the audio data will be decoded to.
  // Must be one of 8000, 12000, 16000, 24000, 48000
  sampleRate: number;
};

type ConsumeReturn = {
  shutdown: () => Promise<void>;
};

export function produceRtp(options: ProduceOptions): ProduceReturn {
  const host = options.ipAddress.includes(":") ? `[${options.ipAddress}]` : options.ipAddress;
  const rtpUrl = `rtp://${host}:${options.rtpPort}`;

  const abortController = new AbortController();

  const { promise, external } = native.startAudioEncodeThread(
    abortController.signal,
    {
      rtpUrl,
      ssrc: String(options.ssrc),
      payloadType: String(options.payloadType),
      cname: options.cname,
      sampleRate: options.sampleRate,
      bitrate: options.opus?.bitrate ?? 0,
      enableFec: options.opus?.enableFec ?? false,
      packetLossPercent: options.opus?.packetLossPercent ?? 0,
      cryptoSuite: options.srtpParameters?.cryptoSuite,
      keyBase64: options.srtpParameters?.keyBase64,
    },
  );

  promise.catch((error: any) => {
    options.onError(error);
  });

  function shutdown(options: { immediate?: boolean } = {}) {
    if (options.immediate) {
      native.clearMessageQueue(external);
      native.postClearProducerQueue(external);
    }

    abortController.abort();
    return promise;
  }

  function sendAudioData(buffer: Buffer): boolean {
    return native.postPcmToEncoder(external, buffer);
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

export function createSrtpParameters(): SrtpParameters {
  return {
    cryptoSuite: "AES_CM_128_HMAC_SHA1_80",
    keyBase64: randomBytes(30).toString("base64"),
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
