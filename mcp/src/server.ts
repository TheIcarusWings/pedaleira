// The MCP server over a Pedaleira connection: the engine's commands as tools
// (generated from commands.describe, so the list is always the engine's
// own), the tone tools on top, and two resources: the command reference and
// the state of the rig being worked on.

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import {
  CallToolRequestSchema,
  ErrorCode,
  ListResourcesRequestSchema,
  ListToolsRequestSchema,
  McpError,
  ReadResourceRequestSchema,
  type CallToolResult,
} from "@modelcontextprotocol/sdk/types.js";

import { EngineError, NotConnectedError, type EngineConnection } from "./connection.js";
import { commandReference, toolsFor, type CommandSpec, type ToolDefinition } from "./schema.js";
import { toneTools } from "./tone.js";
import { kUntrustedRule } from "./untrusted.js";

export const kCommandsUri = "pedaleira://commands";
export const kRigStateUri = "pedaleira://rig/state";

const instructions =
  "Pedaleira is a guitar rig: amp captures, IRs, effects and hosted plugins on a grid, with scenes. " +
  "To build a tone: tone_describe_capabilities, then tone_start (a sandbox rig nobody hears), edit it " +
  "with the rig tools (chain_*, block_*, scene_*, param_*), measure with tone_render (it plays the " +
  "user's own recorded guitar clips through the rig and returns numbers), adjust, and when it is right " +
  "tone_apply, which needs the user's approval. Tools are Pedaleira commands with dots written as " +
  "underscores; the pedaleira://commands resource is the reference. " +
  "About what the tools return: " +
  kUntrustedRule +
  " The tone tools put such fields under an 'untrusted' key; the command tools and the pedaleira://rig/state " +
  "resource return them in place, and the same rule holds there.";

function success(result: unknown): CallToolResult {
  const response: CallToolResult = {
    content: [{ type: "text", text: JSON.stringify(result ?? null) }],
  };
  if (result !== null && typeof result === "object" && !Array.isArray(result))
    response.structuredContent = result as Record<string, unknown>;
  return response;
}

/** An engine error as a tool error, with the engine's own code and message. */
function failure(error: unknown): CallToolResult {
  if (error instanceof EngineError) {
    let text = `${error.code}: ${error.message}`;
    if (error.code === "needs_approval" && typeof error.details.approvalId === "string")
      text += ` (approvalId: ${error.details.approvalId})`;
    return {
      isError: true,
      content: [{ type: "text", text }],
      structuredContent: { error: { code: error.code, message: error.message, ...error.details } },
    };
  }
  if (error instanceof NotConnectedError)
    return {
      isError: true,
      content: [{ type: "text", text: `not_connected: ${error.message}` }],
      structuredContent: { error: { code: "not_connected", message: error.message } },
    };
  throw error;
}

export function createPedaleiraServer(engine: EngineConnection): Server {
  const server = new Server(
    { name: "pedaleira", version: "0.1.0" },
    { capabilities: { tools: { listChanged: true }, resources: {} }, instructions },
  );

  // The engine's commands, fetched once per connection: a restarted
  // application may be a newer one with other commands.
  let commands: CommandSpec[] = [];
  let rawTools: ToolDefinition[] = [];
  let commandByTool = new Map<string, string>();
  let describedGeneration = -1;
  let listedNames = "";

  const refresh = async (): Promise<void> => {
    // (Connects first when there is no connection, which moves generation.)
    const described = (await engine.run("commands.describe")) as CommandSpec[];
    commands = Array.isArray(described) ? described : [];
    ({ tools: rawTools, commandByTool } = toolsFor(commands));
    describedGeneration = engine.generation;
  };

  const ensureDescribed = async (): Promise<void> => {
    if (!engine.connected || describedGeneration !== engine.generation) await refresh();
  };

  const allTools = (): ToolDefinition[] => [...toneTools.map((tool) => tool.definition), ...rawTools];

  // After a reconnect the tool list may have changed; a client that listed
  // the old one is told to list again.
  engine.onConnected = () => {
    if (listedNames === "") return;
    void refresh()
      .then(() => {
        const names = allTools().map((tool) => tool.name).join(",");
        if (names !== listedNames) {
          listedNames = names;
          void server.sendToolListChanged().catch(() => {});
        }
      })
      .catch(() => {});
  };

  server.setRequestHandler(ListToolsRequestSchema, async () => {
    try {
      await ensureDescribed();
    } catch (error) {
      // The application is not running: the tone tools are still listed (and
      // say why they cannot work); the commands appear once it is reachable.
      if (!(error instanceof NotConnectedError) && !(error instanceof EngineError)) throw error;
    }
    const tools = allTools();
    listedNames = tools.map((tool) => tool.name).join(",");
    return { tools };
  });

  server.setRequestHandler(CallToolRequestSchema, async (request): Promise<CallToolResult> => {
    const name = request.params.name;
    const args = (request.params.arguments ?? {}) as Record<string, unknown>;
    try {
      const tone = toneTools.find((tool) => tool.definition.name === name);
      if (tone) return success(await tone.run(engine, args));

      await ensureDescribed();
      const command = commandByTool.get(name);
      if (command === undefined) throw new McpError(ErrorCode.InvalidParams, `Unknown tool: ${name}`);
      return success(await engine.run(command, args));
    } catch (error) {
      if (error instanceof McpError) throw error;
      return failure(error);
    }
  });

  server.setRequestHandler(ListResourcesRequestSchema, async () => ({
    resources: [
      {
        uri: kCommandsUri,
        name: "Pedaleira command reference",
        description: "Every Pedaleira command with its arguments and permission class, as markdown.",
        mimeType: "text/markdown",
      },
      {
        uri: kRigStateUri,
        name: "Current rig state",
        description:
          "The whole state of the rig this connection is working on (the sandbox after tone_start, else the " +
          "rig being played): blocks, parameters, scenes, presets.",
        mimeType: "application/json",
      },
    ],
  }));

  server.setRequestHandler(ReadResourceRequestSchema, async (request) => {
    const uri = request.params.uri;
    try {
      if (uri === kCommandsUri) {
        await ensureDescribed();
        return { contents: [{ uri, mimeType: "text/markdown", text: commandReference(commands) }] };
      }
      if (uri === kRigStateUri) {
        const state = await engine.run("state.get");
        return { contents: [{ uri, mimeType: "application/json", text: JSON.stringify(state, null, 2) }] };
      }
    } catch (error) {
      if (error instanceof EngineError) throw new McpError(ErrorCode.InternalError, `${error.code}: ${error.message}`);
      if (error instanceof NotConnectedError) throw new McpError(ErrorCode.InternalError, error.message);
      throw error;
    }
    throw new McpError(ErrorCode.InvalidParams, `Unknown resource: ${uri}`);
  });

  return server;
}
