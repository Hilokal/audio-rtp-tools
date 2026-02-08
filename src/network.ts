import dgram from "dgram";
import { networkInterfaces } from "os";
function shuffle<T>(array: T[]): T[] {
  for (let i = array.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [array[i], array[j]] = [array[j], array[i]];
  }
  return array;
}

// Environment helpers

export function getEnv(key: string, defaultValue?: string): string {
  const value = process.env[key];
  if (typeof value === "string") {
    return value;
  } else if (defaultValue !== undefined) {
    return defaultValue;
  } else {
    throw new Error(`Missing required env ${key}`);
  }
}

export function getEnvBool(key: string, defaultValue: boolean = false): boolean {
  const value = process.env[key];
  if (value === undefined) {
    return defaultValue;
  }
  const str = value.trim();
  return !(str === "0" || str === "false" || str === "");
}

// Port availability testing

async function testPortAvailability(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const server = dgram.createSocket("udp4");

    server.once("error", function (err: NodeJS.ErrnoException) {
      if (err.code !== "EADDRINUSE") {
        console.warn(`Got error while testing for port ${port}`);
        console.error(err);
      }
      resolve(false);
    });

    server.once("listening", function () {
      server.close(() => {
        resolve(true);
      });
    });

    server.bind(port);
  });
}

// IP address detection

const isUseVPC = getEnvBool("USE_VPC_IP");

function getListenIpInternal(): string {
  const DEFAULT_ADDRESS = "127.0.0.1";

  const interfaces = networkInterfaces();
  const netInterfaceVPC = interfaces.eth1 || interfaces.enp6s0 || interfaces.ens4;
  const netInterface = interfaces.en0 || interfaces.eth0 || interfaces.enp1s0 || interfaces.bond0;

  if (netInterfaceVPC == null && netInterface == null) {
    console.warn("Network device not found. Available interfaces:", Object.keys(interfaces));
    return DEFAULT_ADDRESS;
  } else {
    const ipv4 =
      netInterfaceVPC && isUseVPC
        ? netInterfaceVPC.find((record) => record.family === "IPv4" && !record.internal)
        : netInterface?.find((record) => record.family === "IPv4" && !record.internal);

    if (ipv4 == null) {
      console.warn("No ipv4 address found on network interface");
      return DEFAULT_ADDRESS;
    } else {
      return ipv4.address;
    }
  }
}

let listenIp: string | null = null;

export function getListenIp(): string {
  if (listenIp == null) {
    listenIp = getListenIpInternal();
  }
  return listenIp;
}

export function isLoopback(ipAddress: string) {
  return (
    ipAddress === "::1" ||
    ipAddress === "127.0.0.1" ||
    ipAddress === "localhost" ||
    ipAddress === "::ffff:127.0.0.1"
  );
}
// Port management

const minPort = parseInt(getEnv("MIN_RTP_PORT", "10000"), 10);
const maxPort = parseInt(getEnv("MAX_RTP_PORT", "10100"), 10);

if (minPort >= maxPort) {
  throw new Error("MIN_RTP_PORT must be less than MAX_RTP_PORT");
}

if (minPort % 2 !== 0) {
  // The RTP spec recommends an even number for RTP port values.
  // https://tools.ietf.org/html/rfc3550#section-11
  throw new Error("MIN_RTP_PORT must be an even value.");
}

const maxValidPort = 65353;
if (maxPort > maxValidPort) {
  throw new Error(`MAX_RTP_PORT cannot be greater than ${maxValidPort}. It is set to ${maxPort}`);
}

const availablePortOffsets = shuffle(Array.from({ length: (maxPort - minPort) / 2 }, (_, i) => i));

export async function choosePorts(): Promise<[number, number]> {
  const maxAttempts = 20;

  for (let i = 0; i < maxAttempts; i++) {
    const nextOffset = availablePortOffsets.shift();
    if (nextOffset == null) {
      throw new Error("Ran out of available ports");
    }

    const port1 = minPort + nextOffset * 2;
    const port2 = port1 + 1;

    if (!(await testPortAvailability(port1))) {
      console.log(`Port ${port1} is not available. Trying another port.`);
      releasePorts(port1, port2);
      continue;
    }

    if (!(await testPortAvailability(port2))) {
      console.log(`Port ${port2} is not available. Trying another port.`);
      releasePorts(port1, port2);
      continue;
    }

    return [port1, port2];
  }

  throw new Error(`Gave up trying to pick a port after ${maxAttempts} failed attempts`);
}

export function releasePorts(port1: number, port2: number) {
  if (port2 !== port1 + 1) {
    throw new Error("Expected port1 and port2 to be consecutive");
  }
  availablePortOffsets.push((port1 - minPort) / 2);
}
