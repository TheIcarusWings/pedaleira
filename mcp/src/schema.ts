// From the engine's own description of its commands (commands.describe) to
// MCP tools: names, JSON Schema for the arguments, and descriptions. Nothing
// about any particular command is written down here; a command added to the
// engine shows up as a tool by itself.

import { kUntrustedSuffix } from "./untrusted.js";

export interface CommandParamSpec {
  name: string;
  type: "string" | "number" | "integer" | "boolean" | "object" | "array";
  required: boolean;
  description: string;
  enum?: string[];
  min?: number;
  max?: number;
  items?: string;
  default?: unknown;
  path?: boolean;
}

export interface CommandSpec {
  name: string;
  summary: string;
  mutates: boolean;
  /** read, edit, render, live, delete, settings or local. */
  permission: string;
  params: CommandParamSpec[];
}

export interface JsonSchema {
  type: "object";
  properties: Record<string, Record<string, unknown>>;
  required?: string[];
  additionalProperties: false;
  [key: string]: unknown;
}

export interface ToolDefinition {
  name: string;
  description: string;
  inputSchema: JsonSchema;
}

/** `chain.addBlock` becomes `chain_addBlock`: tool names may not hold dots. */
export function toolNameFor(command: string): string {
  return command.replaceAll(".", "_");
}

const permissionNotes: Record<string, string> = {
  read: "reads only",
  edit: "edits a rig or the library; on the rig being played it also needs the live permission, on a sandbox rig it does not (a sandbox only adds presets; reorganising the library always counts as live)",
  render: "renders audio offline to measure it",
  live: "changes the rig being played; answers needs_approval while that permission is off",
  delete: "deletes; off unless the user switched it on",
  settings: "changes machine-wide settings; off unless the user switched it on",
};

/** The JSON Schema of one command's arguments. */
export function inputSchemaFor(command: CommandSpec): JsonSchema {
  const properties: Record<string, Record<string, unknown>> = {};
  const required: string[] = [];
  for (const param of command.params) {
    const property: Record<string, unknown> = { type: param.type };
    let description = param.description;
    if (param.path) description += " Must lie inside the allowed folders (roots_list).";
    if (description) property.description = description;
    if (param.enum && param.enum.length > 0) property.enum = param.enum;
    if (typeof param.min === "number") property.minimum = param.min;
    if (typeof param.max === "number") property.maximum = param.max;
    if (param.type === "array" && param.items) property.items = { type: param.items };
    if (param.default !== undefined && param.default !== null) property.default = param.default;
    properties[param.name] = property;
    if (param.required) required.push(param.name);
  }
  const schema: JsonSchema = { type: "object", properties, additionalProperties: false };
  if (required.length > 0) schema.required = required;
  return schema;
}

/**
 * The tools for a list of commands. Commands of the `local` class are left
 * out: the command server never runs them for a client, so offering them
 * would only produce refusals.
 */
export function toolsFor(commands: CommandSpec[]): { tools: ToolDefinition[]; commandByTool: Map<string, string> } {
  const tools: ToolDefinition[] = [];
  const commandByTool = new Map<string, string>();
  for (const command of commands) {
    if (command.permission === "local") continue;
    const name = toolNameFor(command.name);
    if (commandByTool.has(name)) continue;
    commandByTool.set(name, command.name);
    const note = permissionNotes[command.permission] ?? command.permission;
    tools.push({
      name,
      // Every command tool says it: which results carry names, notes or
      // metadata is the engine's to change, and a tool that forgot to say so
      // would be the one an injected sentence gets through.
      description: `${command.summary} [Pedaleira command ${command.name}; permission: ${command.permission} (${note})] ${kUntrustedSuffix}`,
      inputSchema: inputSchemaFor(command),
    });
  }
  return { tools, commandByTool };
}

/** The command reference as markdown, for the pedaleira://commands resource. */
export function commandReference(commands: CommandSpec[]): string {
  const lines: string[] = [
    "# Pedaleira commands",
    "",
    "Each command is available as a tool whose name is the command's with dots written as",
    "underscores (chain.addBlock is chain_addBlock). Rig commands act on the rig the connection",
    "has selected: the rig being played, or a sandbox rig after tone_start / sandbox_create.",
    "",
    "| Command | Permission | Summary |",
    "|---|---|---|",
  ];
  for (const command of commands)
    lines.push(`| \`${command.name}\` | ${command.permission} | ${command.summary.replaceAll("|", "\\|")} |`);
  for (const command of commands) {
    lines.push("", `## ${command.name}`, "", command.summary, "", `Permission: ${command.permission}.`);
    if (command.params.length === 0) {
      lines.push("", "No arguments.");
      continue;
    }
    lines.push("", "| Argument | Type | Required | Description |", "|---|---|---|---|");
    for (const param of command.params) {
      const extras: string[] = [];
      if (param.enum?.length) extras.push(`one of ${param.enum.join(", ")}`);
      if (typeof param.min === "number" || typeof param.max === "number")
        extras.push(`${param.min ?? ""} to ${param.max ?? ""}`.trim());
      if (param.default !== undefined && param.default !== null) extras.push(`default ${JSON.stringify(param.default)}`);
      const description = [param.description, ...extras].filter(Boolean).join("; ").replaceAll("|", "\\|");
      const type = param.type === "array" && param.items ? `array of ${param.items}` : param.type;
      lines.push(`| \`${param.name}\` | ${type} | ${param.required ? "yes" : "no"} | ${description} |`);
    }
  }
  return lines.join("\n") + "\n";
}
