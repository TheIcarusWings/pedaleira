// What the tests share: a PedaleiraHeadless process to talk to (the real
// engine with its command server, in scratch folders), an MCP client wired
// to the server under test, and a way to make guitar clips.

import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { existsSync } from "node:fs";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";

import { EngineConnection } from "../src/connection.js";
import { createPedaleiraServer } from "../src/server.js";

// dist/test/helpers.js -> the repository root is three levels up.
export const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");
export const testFiles = join(repoRoot, "test", "files");

function headlessBinary(): string {
  const name = process.platform === "win32" ? "PedaleiraHeadless.exe" : "PedaleiraHeadless";
  const candidates = [
    process.env.PEDALEIRA_HEADLESS,
    join(repoRoot, "build", "tools", "PedaleiraHeadless_artefacts", "Release", name),
    join(repoRoot, "build", "tools", "PedaleiraHeadless_artefacts", "Debug", name),
    join(repoRoot, "build", "tools", "PedaleiraHeadless_artefacts", name),
  ].filter((path): path is string => typeof path === "string" && path.length > 0);
  const found = candidates.find((path) => existsSync(path));
  if (!found)
    throw new Error(
      "PedaleiraHeadless is not built. Run `cmake --build build --target PedaleiraHeadless` in the repository " +
        `root, or set PEDALEIRA_HEADLESS to the binary. Looked in:\n  ${candidates.join("\n  ")}`,
    );
  return found;
}

export interface HostInfo {
  connectionFile: string;
  port: number;
  data: string;
  library: string;
}

/** A running PedaleiraHeadless. Its stdin is the user's side of the application. */
export class HeadlessHost {
  private constructor(
    private readonly child: ChildProcessWithoutNullStreams,
    readonly info: HostInfo,
    private readonly lines: AsyncIterableIterator<string>,
  ) {}

  static async start(dataDir: string, extraArgs: string[] = []): Promise<HeadlessHost> {
    const child = spawn(headlessBinary(), ["--data", dataDir, "--allow-root", testFiles, ...extraArgs], {
      stdio: ["pipe", "pipe", "pipe"],
    });
    child.stderr.resume();
    const lines = createInterface({ input: child.stdout })[Symbol.asyncIterator]();
    const first = await Promise.race([
      lines.next(),
      new Promise<never>((_, reject) => child.once("exit", (code) => reject(new Error(`PedaleiraHeadless exited with ${code}`)))),
    ]);
    const ready = JSON.parse(String(first.value)) as HostInfo & { ready: boolean };
    if (!ready.ready) throw new Error("PedaleiraHeadless did not report ready");
    return new HeadlessHost(child, ready, lines);
  }

  /** Runs a command as the local user (no permission gate) and returns its result. */
  async local(command: string, args: Record<string, unknown> = {}): Promise<any> {
    this.child.stdin.write(JSON.stringify({ command, args }) + "\n");
    const line = await this.lines.next();
    const answer = JSON.parse(String(line.value)) as { ok: boolean; result?: unknown; error?: { code: string; message: string } };
    if (!answer.ok) throw new Error(`${command}: ${answer.error?.code}: ${answer.error?.message}`);
    return answer.result;
  }

  async stop(): Promise<void> {
    if (this.child.exitCode !== null) return;
    const exited = new Promise<void>((done) => this.child.once("exit", () => done()));
    this.child.stdin.write("exit\n");
    const timer = setTimeout(() => this.child.kill("SIGKILL"), 15_000);
    await exited;
    clearTimeout(timer);
  }
}

/** A scratch data folder for one host, removed by `dispose`. */
export async function scratchDir(): Promise<{ dir: string; dispose: () => Promise<void> }> {
  const dir = await mkdtemp(join(tmpdir(), "pedaleira-mcp-test-"));
  return { dir, dispose: () => rm(dir, { recursive: true, force: true }) };
}

/** An MCP client connected, in process, to the server under test. */
export async function connectClient(connectionFile: string, clientName = "mcp test") {
  const engine = new EngineConnection({ connectionFile, clientName, requestTimeoutMs: 120_000 });
  const server = createPedaleiraServer(engine);
  const [clientSide, serverSide] = InMemoryTransport.createLinkedPair();
  const client = new Client({ name: "pedaleira-mcp-tests", version: "0.0.0" }, { capabilities: {} });
  await server.connect(serverSide);
  await client.connect(clientSide);
  return {
    client,
    engine,
    server,
    async close() {
      await client.close();
      engine.close();
    },
  };
}

/** Calls a tool and returns its JSON result; throws when the tool reported an error. */
export async function call(client: Client, name: string, args: Record<string, unknown> = {}): Promise<any> {
  const result = await client.callTool({ name, arguments: args });
  const text = (result.content as Array<{ type: string; text?: string }>)[0]?.text ?? "";
  if (result.isError) throw new ToolError(text, result.structuredContent as Record<string, any> | undefined);
  return JSON.parse(text);
}

export class ToolError extends Error {
  constructor(
    message: string,
    readonly structured: Record<string, any> | undefined,
  ) {
    super(message);
  }
  get code(): string {
    return this.structured?.error?.code ?? "";
  }
}

/** Calls a tool that is expected to fail and returns the error. */
export async function callExpectingError(client: Client, name: string, args: Record<string, unknown> = {}): Promise<ToolError> {
  try {
    await call(client, name, args);
  } catch (error) {
    if (error instanceof ToolError) return error;
    throw error;
  }
  throw new Error(`${name} succeeded where an error was expected`);
}

/** Plucked notes, a few harmonics each under a decaying envelope: guitar enough for a rig. */
export function pluckedClip(seconds: number, amplitude: number, sampleRate = 48000): Float32Array {
  const frames = Math.floor(seconds * sampleRate);
  const noteFrames = sampleRate / 2;
  const notes = [110, 146.83, 196, 164.81];
  const samples = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    const f0 = notes[Math.floor(i / noteFrames) % 4]!;
    const t = (i % noteFrames) / sampleRate;
    let v = 0;
    let norm = 0;
    for (let h = 1; h <= 10; h++) {
      const weight = Math.pow(h, -1.1);
      v += weight * Math.sin(2 * Math.PI * f0 * h * t) * Math.exp(-t * (3 + h));
      norm += weight;
    }
    samples[i] = amplitude * Math.min(1, t * 400) * (v / norm) * 2;
  }
  return samples;
}

/** Writes a mono 16-bit WAV. */
export async function writeWav(path: string, samples: Float32Array, sampleRate = 48000): Promise<string> {
  await mkdir(dirname(path), { recursive: true });
  const data = Buffer.alloc(44 + samples.length * 2);
  data.write("RIFF", 0);
  data.writeUInt32LE(36 + samples.length * 2, 4);
  data.write("WAVEfmt ", 8);
  data.writeUInt32LE(16, 16);
  data.writeUInt16LE(1, 20); // PCM
  data.writeUInt16LE(1, 22); // mono
  data.writeUInt32LE(sampleRate, 24);
  data.writeUInt32LE(sampleRate * 2, 28);
  data.writeUInt16LE(2, 32);
  data.writeUInt16LE(16, 34);
  data.write("data", 36);
  data.writeUInt32LE(samples.length * 2, 40);
  for (let i = 0; i < samples.length; i++)
    data.writeInt16LE(Math.round(Math.max(-1, Math.min(1, samples[i]!)) * 32767), 44 + i * 2);
  await writeFile(path, data);
  return path;
}

/** A guitar profile with a soft and a hard take, made through the tools. */
export async function makeGuitar(client: Client, library: string, name = "Test Guitar"): Promise<string> {
  const created = await call(client, "guitar_create", { name, positions: ["bridge"], pickupType: "humbucker" });
  for (const [take, amplitude] of [["soft", 0.1], ["hard", 0.8]] as const) {
    const path = await writeWav(join(library, "incoming", `${take}.wav`), pluckedClip(2, amplitude));
    await call(client, "guitar_importClip", { guitar: created.id, position: "bridge", take, path });
  }
  return created.id as string;
}
