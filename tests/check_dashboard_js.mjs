import fs from "node:fs";

const source = fs.readFileSync(
  new URL("../components/pb_portal/pb_portal.c", import.meta.url),
  "utf8",
);
const start = source.indexOf("static const char STATUS_BODY[]");
const end = source.indexOf("// Dedicated firmware-update page", start);
if (start < 0 || end < 0) throw new Error("STATUS_BODY not found");

const block = source.slice(start, end);
const tokens = block.match(/"(?:\\.|[^"\\])*"/gs) ?? [];
const html = tokens.map((token) => Function(`return ${token}`)()).join("");
const script = html.match(/<script>([\s\S]*)<\/script>/)?.[1];
if (!script) throw new Error("dashboard script not found");

// DB_AUTH_JS is a C macro between string tokens, so its helpers are absent from
// the extracted text. They are runtime dependencies, not syntax dependencies.
Function("tok", "hdr", script);
console.log("dashboard JavaScript syntax: PASS");
