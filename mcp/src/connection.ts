// The connection to Pedaleira's command server: one line of JSON per request
// and per answer over a local TCP socket (docs/agents.md in the repository
// has the protocol). The port and the token come from the connection file
// the application writes when its command server is switched on.
//
// The connection is made on first use and made again whenever it has gone:
// the application restarting gives it a new port and a new token, so every
// reconnect reads the file afresh.

import { readFile } from "node:fs/promises";
import { createConnection, type Socket } from "node:net";
import { homedir } from "node:os";
import { join } from "node:path";

/** What an engine command answered with when it failed. */
export class EngineError extends Error {
  constructor(
    readonly code: string,
    message: string,
    /** Extra fields of the error object (needs_approval's approvalId). */
    readonly details: Record<string, unknown> = {},
  ) {
    super(message);
    this.name = "EngineError";
  }
}

/** The application is not running, its server is off, or the link broke. */
export class NotConnectedError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "NotConnectedError";
  }
}

interface ConnectionInfo {
  host: string;
  port: number;
  token: string;
  protocol: number;
  pid?: number;
}

/**
 * Whether a process with that id exists, as far as this one can tell. Signal
 * 0 delivers nothing and only reports: "no such process" is the one answer
 * that means gone; "not permitted" means it is there and someone else's.
 */
export function processIsAlive(pid: number): boolean {
  if (!Number.isInteger(pid) || pid <= 0) return true; // nothing to go by
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return (error as NodeJS.ErrnoException).code !== "ESRCH";
  }
}

interface Pending {
  resolve: (value: unknown) => void;
  reject: (error: Error) => void;
  timer: NodeJS.Timeout;
}

export interface EngineConnectionOptions {
  /** Path of the connection file; default: where the application writes it. */
  connectionFile?: string;
  /** The name this client declares; shown in the application's client list. */
  clientName?: string;
  /** How long one request may take, in milliseconds. Renders are slow. */
  requestTimeoutMs?: number;
}

/** Where the application writes its connection file on this platform. */
export function defaultConnectionFile(): string {
  if (process.platform === "darwin")
    return join(homedir(), "Library", "Application Support", "Pedaleira", "command-server.json");
  if (process.platform === "win32")
    return join(process.env.APPDATA ?? join(homedir(), "AppData", "Roaming"), "Pedaleira", "command-server.json");
  return join(process.env.XDG_CONFIG_HOME ?? join(homedir(), ".config"), "Pedaleira", "command-server.json");
}

// The server caps a request line at one megabyte; an answer (a full state
// with embedded tone data, a long listing) can be larger, but not without
// bound.
const kMaxLineBytes = 64 * 1024 * 1024;

export class EngineConnection {
  readonly connectionFile: string;
  readonly clientName: string;
  private readonly requestTimeoutMs: number;

  private socket: Socket | null = null;
  private connecting: Promise<void> | null = null;
  private buffer = "";
  private nextId = 1;
  private readonly pending = new Map<number, Pending>();
  private generationCounter = 0;

  /** Called after each successful (re)connect. */
  onConnected: (() => void) | null = null;
  /** Called with each event line the server pushes. */
  onEvent: ((event: Record<string, unknown>) => void) | null = null;

  constructor(options: EngineConnectionOptions = {}) {
    this.connectionFile =
      options.connectionFile ?? process.env.PEDALEIRA_CONNECTION_FILE ?? defaultConnectionFile();
    this.clientName = options.clientName ?? process.env.PEDALEIRA_CLIENT_NAME ?? "MCP client";
    this.requestTimeoutMs = options.requestTimeoutMs ?? 180_000;
  }

  get connected(): boolean {
    return this.socket !== null;
  }

  /** Counts the connections made so far; changes when a reconnect happened. */
  get generation(): number {
    return this.generationCounter;
  }

  /** Runs one engine command and returns its result. */
  async run(command: string, args: Record<string, unknown> = {}): Promise<unknown> {
    await this.ensureConnected();
    const socket = this.socket;
    if (socket === null) throw new NotConnectedError("the connection to Pedaleira closed");
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new NotConnectedError(`Pedaleira did not answer '${command}' within ${this.requestTimeoutMs} ms`));
      }, this.requestTimeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      socket.write(JSON.stringify({ id, command, args }) + "\n");
    });
  }

  close(): void {
    this.drop(new NotConnectedError("the connection was closed"));
  }

  private async ensureConnected(): Promise<void> {
    if (this.socket !== null) return;
    this.connecting ??= this.connect().finally(() => {
      this.connecting = null;
    });
    await this.connecting;
  }

  private async readConnectionInfo(): Promise<ConnectionInfo> {
    let text: string;
    try {
      text = await readFile(this.connectionFile, "utf8");
    } catch {
      throw new NotConnectedError(
        `Pedaleira is not reachable: no connection file at ${this.connectionFile}. ` +
          "Start Pedaleira and switch its command server on (it is off by default), " +
          "or run PedaleiraHeadless.",
      );
    }
    let info: Partial<ConnectionInfo>;
    try {
      info = JSON.parse(text) as Partial<ConnectionInfo>;
      if (typeof info.port !== "number" || typeof info.token !== "string") throw new Error("incomplete");
      // The server only ever listens on the loopback interface; a file that
      // says otherwise is not one of ours.
      if (info.host !== undefined && info.host !== "127.0.0.1")
        throw new Error("not a local address");
    } catch (error) {
      throw new NotConnectedError(
        `the connection file ${this.connectionFile} is not valid (${(error as Error).message})`,
      );
    }
    // The file names the process that wrote it. When that process has gone
    // (it crashed before it could remove the file), the port is nobody's, or
    // by now somebody else's: say so instead of sending the token there.
    // Best effort: a process id that has been given out again looks alive.
    if (typeof info.pid === "number" && !processIsAlive(info.pid))
      throw new NotConnectedError(
        `Pedaleira is not running: the connection file ${this.connectionFile} was left by a process ` +
          `(pid ${info.pid}) that has gone. Start Pedaleira and switch its command server on, or run PedaleiraHeadless.`,
      );
    return { host: "127.0.0.1", port: info.port, token: info.token, protocol: info.protocol ?? 1, pid: info.pid };
  }

  private async connect(): Promise<void> {
    const info = await this.readConnectionInfo();
    const socket = await new Promise<Socket>((resolve, reject) => {
      const candidate = createConnection({ host: info.host, port: info.port });
      candidate.once("connect", () => resolve(candidate));
      candidate.once("error", (error) =>
        reject(
          new NotConnectedError(
            `Pedaleira is not reachable on port ${info.port} (${error.message}); ` +
              "it may have quit, leaving a stale connection file.",
          ),
        ),
      );
    });
    socket.setEncoding("utf8");
    socket.setNoDelay(true);

    // The token first; the server answers with one line and, when the token
    // is wrong, closes.
    const hello = await new Promise<Record<string, unknown>>((resolve, reject) => {
      let received = "";
      const onData = (chunk: string) => {
        received += chunk;
        const end = received.indexOf("\n");
        if (end < 0) return;
        socket.off("data", onData);
        socket.off("close", onClose);
        this.buffer = received.slice(end + 1);
        try {
          resolve(JSON.parse(received.slice(0, end)) as Record<string, unknown>);
        } catch {
          reject(new NotConnectedError("Pedaleira answered the handshake with something that is not JSON"));
        }
      };
      const onClose = () => reject(new NotConnectedError("Pedaleira closed the connection during the handshake"));
      socket.on("data", onData);
      socket.once("close", onClose);
      socket.write(JSON.stringify({ auth: info.token, client: this.clientName }) + "\n");
    }).catch((error: Error) => {
      socket.destroy();
      throw error;
    });
    if (hello.ok !== true) {
      socket.destroy();
      const error = hello.error as { message?: string } | undefined;
      throw new NotConnectedError(`Pedaleira refused the connection: ${error?.message ?? "unauthorized"}`);
    }

    socket.on("data", (chunk: string) => this.receive(chunk));
    socket.on("close", () => {
      if (this.socket === socket) this.drop(new NotConnectedError("the connection to Pedaleira closed"));
    });
    socket.on("error", () => {
      // "close" follows and does the work.
    });
    this.socket = socket;
    this.generationCounter += 1;
    // Lines that arrived with the handshake answer.
    this.receive("");
    this.onConnected?.();
  }

  private receive(chunk: string): void {
    this.buffer += chunk;
    if (this.buffer.length > kMaxLineBytes) {
      this.drop(new NotConnectedError("Pedaleira sent a line too long to be an answer"));
      return;
    }
    for (;;) {
      const end = this.buffer.indexOf("\n");
      if (end < 0) return;
      const line = this.buffer.slice(0, end);
      this.buffer = this.buffer.slice(end + 1);
      if (line.trim() === "") continue;
      let message: Record<string, unknown>;
      try {
        message = JSON.parse(line) as Record<string, unknown>;
      } catch {
        continue;
      }
      if (typeof message.event === "string") {
        this.onEvent?.(message);
        continue;
      }
      const id = message.id;
      if (typeof id !== "number") {
        // An answer without an id is the server saying why it is about to
        // close the connection (rate limit, a line too long).
        const error = message.error as { code?: string; message?: string } | undefined;
        if (error) this.drop(new NotConnectedError(`Pedaleira closed the connection: ${error.message ?? error.code}`));
        continue;
      }
      const waiting = this.pending.get(id);
      if (!waiting) continue;
      this.pending.delete(id);
      clearTimeout(waiting.timer);
      if (message.ok === true) {
        waiting.resolve(message.result);
      } else {
        const { code, message: text, ...details } = (message.error ?? {}) as Record<string, unknown>;
        waiting.reject(
          new EngineError(
            typeof code === "string" ? code : "failed",
            typeof text === "string" ? text : "the command failed",
            details,
          ),
        );
      }
    }
  }

  private drop(reason: Error): void {
    const socket = this.socket;
    this.socket = null;
    this.buffer = "";
    socket?.destroy();
    for (const [, waiting] of this.pending) {
      clearTimeout(waiting.timer);
      waiting.reject(reason);
    }
    this.pending.clear();
  }
}
