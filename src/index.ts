const native = require("../build/Release/worker.node");
import { isIPv6 } from "net";
import { randomBytes, getRandomValues } from "crypto";

// Taken from mediasoup source
// https://github.com/versatica/mediasoup/blob/v3/node/src/rtpParametersTypes.ts
import { RtpParameters } from "./rtpParametersTypes";

export type SrtpParameters = {
  cryptoSuite: string;
  keyBase64: string;
};

type ProduceOptions = {
  ipAddress: string;
  rtpParameters: RtpParameters;
  rtpPort: number;
  rtcpPort: number;

  // sample rate of the pcm audio data that will be passed into the write function
  sampleRate: number;

  onError?: (error: Error) => void;

  // Use this to enable encryption. This is the result of the createSrtpParameters function.
  srtpParameters?: SrtpParameters;

  // If this signal is raised, the sending thread will shutdown immediately.
  signal?: AbortSignal;

  opus?: {
    bitrate?: number | null;
    enableFec?: boolean;
    packetLossPercent?: number;
  };
};

type ProduceReturn = {
  // Queues up PCM data to be sent
  write: (data: Buffer) => boolean;

  // Signal the end of a contiguous audio segment. Flushes any partial frame
  // and resets timing so the next write() starts a fresh segment.
  endSegment: () => void;

  // Called when you are done sending data. The thread will shutdown when it's
  // finished sending any queued data.
  end: () => void;

  // Await this to wait until the thread is finished. The thread will finished when one of the following happens:
  // - end() is called and the thread has finished broadcasting the RTP packets
  // - signal() is aborted, and the thread will exit immediately
  // - An error is raised
  done: () => Promise<void>;

  setBitrate: (bitrate: number | null) => void;
  setEnableFec: (enableFec: boolean) => void;
  setPacketLossPercent: (percent: number) => void;
};

type ConsumeOptions = {
  sdp: string;
  onAudioData: (data: { buffer: Buffer; pts: number | null }) => void;
  onError?: (error: Error) => void;

  // Sample rate that the audio data will be decoded to.
  // Must be one of 8000, 12000, 16000, 24000, 48000
  sampleRate: number;

  signal: AbortSignal;
};

type ConsumeReturn = {
  done: () => Promise<void>;
};

export function produceRtp(options: ProduceOptions): ProduceReturn {
  const { rtpParameters, srtpParameters, signal } = options;

  const host = options.ipAddress.includes(":")
    ? `[${options.ipAddress}]`
    : options.ipAddress;

  const protocol = options.srtpParameters ? "srtp://" : "rtp://";
  const rtpUrl = `${protocol}${host}:${options.rtpPort}`;

  // TODO: We could probably support disabling rtcp
  if (rtpParameters.rtcp == null) {
    throw new Error("rtcp parameters are required");
  }
  const cname = rtpParameters.rtcp.cname;

  if (cname == null) {
    throw new Error("expected cname");
  }

  if (
    rtpParameters.codecs.length !== 1 ||
    rtpParameters.codecs[0].mimeType !== "audio/opus"
  ) {
    throw new Error("expected one opus codec");
  }

  const payloadType = rtpParameters.codecs[0].payloadType;

  if (rtpParameters.encodings == null || rtpParameters.encodings.length !== 1) {
    throw new Error("expected one encoding");
  }

  const ssrc = rtpParameters.encodings[0].ssrc;

  const { promise, external } = native.startAudioEncodeThread(signal, {
    rtpUrl,
    ssrc: String(ssrc),
    payloadType: String(payloadType),
    cname: cname,
    sampleRate: options.sampleRate,
    bitrate: options.opus?.bitrate ?? 0,
    enableFec: options.opus?.enableFec ?? false,
    packetLossPercent: options.opus?.packetLossPercent ?? 0,
    cryptoSuite: srtpParameters?.cryptoSuite,
    keyBase64: srtpParameters?.keyBase64,
  });

  if (options.onError) {
    promise.catch((error: any) => {
      options.onError!(error);
    });
  }

  function end() {
    native.postEndOfFile(external);
  }

  function done(): Promise<void> {
    return promise;
  }

  if (signal) {
    function listener() {
      native.clearMessageQueue(external);
      native.postClearProducerQueue(external);
    }

    signal.addEventListener("abort", listener, { once: true });

    promise.then(
      () => {
        signal.removeEventListener("abort", listener);
      },
      () => {
        /* Ignoring error. Just don't want a danging promise */
      },
    );
  }

  function write(buffer: Buffer): boolean {
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

  function endSegment() {
    native.postFlushEncoder(external);
  }

  return {
    end,
    done,
    write,
    endSegment,
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

export function createRtpParameters(): RtpParameters {
  const cname = randomBytes(8).toString("hex");
  const list = new Int32Array(1);
  getRandomValues(list);
  const ssrc = Math.abs(list[0]);

  // WebRTC uses 111 for Opus
  const payloadType = 111;

  return {
    codecs: [
      {
        mimeType: "audio/opus",
        payloadType,
        clockRate: 48000,
        channels: 2,
        parameters: {
          minptime: 10,
          useinbandfec: 1,
        },
        rtcpFeedback: [],
      },
    ],
    headerExtensions: [],
    encodings: [{ ssrc }],
    rtcp: {
      cname,
      reducedSize: true,
    },
  };
}

export function createSDP({
  userName,
  subject,
  sessionId,
  originIpAddress,
  destinationIpAddress,
  rtpPort,
  rtcpPort,
  rtpParameters,
  language,
  srtpParameters,
}: {
  userName?: string;
  subject?: string;
  sessionId?: string;
  originIpAddress?: string;
  destinationIpAddress: string;
  rtpPort: number;
  rtcpPort?: number;
  rtpParameters: RtpParameters;
  language?: string;
  srtpParameters?: SrtpParameters;
}): string {
  if (originIpAddress == null) {
    originIpAddress = "127.0.0.1";
  }

  if (sessionId == null) {
    sessionId = randomBytes(8).toString("hex");
  }

  if (subject == null) {
    subject = "audio-rtp-tools";
  }

  if (userName == null) {
    userName = "-";
  }

  const mediaProtocol = srtpParameters ? "RTP/SAVPF" : "RTP/AVPF";

  const extra: Array<string> = [];

  if (rtcpPort != null) {
    extra.push(`a=rtcp:${rtcpPort}`);
  }

  for (const codec of rtpParameters.codecs) {
    const match = codec.mimeType.match(/(\w+)\/(\w+)/);
    if (!match) {
      throw new Error(`Unexpected mimeType: ${codec.mimeType}`);
    }

    const mediaType = match[1];
    const codecName = match[2];

    extra.push(
      `m=${mediaType} ${rtpPort} ${mediaProtocol} ${codec.payloadType}`,
    );
    extra.push(
      `a=rtpmap:${codec.payloadType} ${codecName}/${codec.clockRate}/${codec.channels}`,
    );

    if (codec.parameters) {
      const fmtp = sdpParameters(codec.parameters);
      extra.push(`a=fmtp:${codec.payloadType} ${fmtp}`);
    }
  }

  if (rtpParameters.headerExtensions) {
    for (const ext of rtpParameters.headerExtensions) {
      let line = `a=extmap:${ext.id} ${ext.uri}`;
      if (ext.parameters) {
        line += ` ${sdpParameters(ext.parameters)}`;
      }
      extra.push(line);
    }
  }

  if (srtpParameters) {
    extra.push(
      `a=crypto:1 ${srtpParameters.cryptoSuite} inline:${srtpParameters.keyBase64}`,
    );
  }

  if (language) {
    extra.push(`a=lang:${language}`);
  }

  const originAddressType = isIPv6(originIpAddress) ? "IP6" : "IP4";
  const destinationAddressType = isIPv6(destinationIpAddress) ? "IP6" : "IP4";

  return `v=0
o=${userName} ${sessionId} 0 IN ${originAddressType} ${originIpAddress}
s=${subject}
c=IN ${destinationAddressType} ${destinationIpAddress}
t=0 0
${extra.join("\n")}
`;
}

function sdpParameters(parameters: Record<string, unknown>): string {
  return Object.entries(parameters)
    .map(([key, value]) => `${key}=${value}`)
    .join(";");
}

export function consumeRtp(options: ConsumeOptions): ConsumeReturn {
  const { promise } = native.startAudioDecodeThread(
    dataUrl(options.sdp),
    options.onAudioData,
    options.signal,
    {
      sampleRate: options.sampleRate,
      channels: 1,
    },
  );

  if (options.onError) {
    promise.catch((error: any) => {
      options.onError!(error);
    });
  }

  function done() {
    return promise;
  }

  return { done };
}

function dataUrl(input: string) {
  return "data:application/sdp;base64," + Buffer.from(input).toString("base64");
}
