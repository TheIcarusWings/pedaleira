#!/usr/bin/env node
// pedaleira-mcp: the stdio MCP server for Pedaleira. An MCP client starts it as
// a child process; it connects to the running application (or to
// PedaleiraHeadless) through the local command server.
//
// Environment:
//   PEDALEIRA_CONNECTION_FILE  path of the connection file (default: where the
//                            application writes it)
//   PEDALEIRA_CLIENT_NAME      the name shown in the application's client list
//
// Standard output belongs to the protocol; anything for a person goes to
// standard error.

import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";

import { EngineConnection } from "./connection.js";
import { createPedaleiraServer } from "./server.js";

const engine = new EngineConnection();
const server = createPedaleiraServer(engine);

const shutdown = () => {
  engine.close();
  void server.close().finally(() => process.exit(0));
};
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

await server.connect(new StdioServerTransport());
console.error(`pedaleira-mcp: serving over stdio; connection file ${engine.connectionFile}`);
