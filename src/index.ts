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

  // Local UDP ports to bind for sending RTP/RTCP. Defaults to ephemeral
  // ports chosen by the OS.
  localRtpPort?: number;
  localRtcpPort?: number;

  // sample rate of the pcm audio data that will be passed into the write function
  sampleRate: number;

  onError?: (error: Error) => void;

  // Called when the encoder's message queue has room after write() returned false.
  // Use this to implement backpressure: stop writing when write() returns false,
  // resume when onDrain fires.
  onDrain?: () => void;

  // Depth of the encoder's message queue. When the queue is full, write() returns
  // false. Smaller values surface backpressure sooner. Defaults to 8192.
  queueDepth?: number;

  // Use this to enable encryption. This is the result of the createSrtpParameters function.
  srtpParameters?: SrtpParameters;

  // If this signal is raised, the sending thread will shutdown immediately.
  signal?: AbortSignal;

  // Send a 1-byte Opus DTX keep-alive packet roughly every 400ms while no
  // audio is being written. Keeps NAT bindings, RTCP, and the receiver's
  // jitter buffer warm during long gaps between segments. Defaults to true;
  // pass false if the receiver should see true silence between segments.
  keepAlive?: boolean;

  // Local encoder tuning that has no representation in rtpParameters.
  // FEC and DTX are NOT set here — they are read from the opus codec
  // parameters in rtpParameters (useinbandfec and usedtx), so the encoder
  // always matches what the stream signals. See createRtpParameters().
  opus?: {
    bitrate?: number | null;
    packetLossPercent?: number;
  };
};

type ProduceReturn = {
  // Queues up PCM data to be sent. Returns true if the data was accepted, false
  // if the queue was full and the data was dropped. When false is returned, wait
  // for the onDrain callback and retry.
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
  setEnableDtx: (enableDtx: boolean) => void;
  setPacketLossPercent: (percent: number) => void;
};

type ConsumeOptions = {
  sdp: string;
  onAudioData: (data: {
    buffer: Buffer;
    pts: number | null;
    // True on the first frame after a timestamp gap longer than the decoder
    // conceals (3s): the frames before it end more than that earlier in the
    // stream. Short gaps are filled with packet-loss concealment and carry
    // no marker.
    discontinuity: boolean;
  }) => void;
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

  const urlParams = new URLSearchParams();
  if (options.localRtpPort != null) {
    urlParams.set("localrtpport", String(options.localRtpPort));
  }
  if (options.localRtcpPort != null) {
    urlParams.set("localrtcpport", String(options.localRtcpPort));
  }
  const query = urlParams.toString();
  const rtpUrl = `${protocol}${host}:${options.rtpPort}${query ? `?${query}` : ""}`;

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

  // FEC and DTX are driven by the signaled codec parameters so the encoder
  // never diverges from what the rtpParameters advertise.
  const codecParameters = rtpParameters.codecs[0].parameters ?? {};
  const enableFec = Number(codecParameters.useinbandfec) === 1;
  const enableDtx = Number(codecParameters.usedtx) === 1;

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
    enableFec,
    enableDtx,
    packetLossPercent: options.opus?.packetLossPercent ?? 0,
    cryptoSuite: srtpParameters?.cryptoSuite,
    keyBase64: srtpParameters?.keyBase64,
    onDrain: options.onDrain,
    queueDepth: options.queueDepth ?? 0,
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

  if (options.keepAlive !== false) {
    // The producer thread only emits a keep-alive if the stream has actually
    // been quiet, so ticking blindly here is safe.
    const keepAliveTimer = setInterval(() => {
      native.postKeepAliveTick(external);
    }, 400);
    keepAliveTimer.unref();

    const clear = () => clearInterval(keepAliveTimer);
    promise.then(clear, clear);
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

  function setEnableDtx(enableDtx: boolean) {
    native.postSetEnableDtx(external, enableDtx);
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
    setEnableDtx,
    setPacketLossPercent,
  };
}

export function createSrtpParameters(): SrtpParameters {
  return {
    cryptoSuite: "AES_CM_128_HMAC_SHA1_80",
    keyBase64: randomBytes(30).toString("base64"),
  };
}

export function createRtpParameters(options?: {
  // Signal (and enable) Opus in-band forward error correction. Default true.
  enableFec?: boolean;

  // Signal (and enable) Opus DTX: while the input PCM is silent, packets are
  // suppressed except a comfort-noise refresh roughly every 400ms, mirroring
  // WebRTC's behavior. Also sets dtx on the encoding, which disables
  // mediasoup's RTP inactivity checks. Default false.
  enableDtx?: boolean;
}): RtpParameters {
  const enableFec = options?.enableFec ?? true;
  const enableDtx = options?.enableDtx ?? false;

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
          useinbandfec: enableFec ? 1 : 0,
          ...(enableDtx ? { usedtx: 1 } : {}),
        },
        rtcpFeedback: [],
      },
    ],
    headerExtensions: [],
    encodings: [enableDtx ? { ssrc, dtx: true } : { ssrc }],
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

type SaveAudioOptions = {
  sdp: string;
  outputPath: string;
  signal: AbortSignal;
  onError?: (error: Error) => void;
};

type SaveAudioReturn = {
  done: () => Promise<void>;
};

export function saveAudio(options: SaveAudioOptions): SaveAudioReturn {
  const { promise } = native.startSaveAudioThread(
    { sdp: dataUrl(options.sdp), outputPath: options.outputPath },
    options.signal,
  );

  if (options.onError) {
    promise.catch((error: any) => {
      options.onError!(error);
    });
  }

  return { done: () => promise };
}

function dataUrl(input: string) {
  return "data:application/sdp;base64," + Buffer.from(input).toString("base64");
}
