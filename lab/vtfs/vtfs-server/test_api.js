// Usage:
//   node test_api.js ping
//   node test_api.js list parent=1000
//   node test_api.js lookup parent=1000 name=dir
// Env:
//   HOST=127.0.0.1 PORT=8080 TOKEN=TODO node test_api.js list parent=1000

const http = require("http");

const HOST = process.env.HOST || "127.0.0.1";
const PORT = Number(process.env.PORT || 8080);
const TOKEN = process.env.TOKEN || "TODO";

function parseArgs(argv) {
  const out = {};
  for (const s of argv) {
    const i = s.indexOf("=");
    if (i === -1) continue;
    out[s.slice(0, i)] = s.slice(i + 1);
  }
  return out;
}

function readAll(res) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    res.on("data", (c) => chunks.push(c));
    res.on("end", () => resolve(Buffer.concat(chunks)));
    res.on("error", reject);
  });
}

function int64LE(buf) {
  if (buf.length < 8) return null;
  return Number(buf.readBigInt64LE(0));
}

function bufRepr(b) {
  // show \0 etc in a compact way
  let s = "";
  for (const x of b) {
    if (x === 0) s += "\\0";
    else if (x === 10) s += "\\n";
    else if (x === 13) s += "\\r";
    else if (x === 9) s += "\\t";
    else if (x >= 32 && x <= 126) s += String.fromCharCode(x);
    else s += "\\x" + x.toString(16).padStart(2, "0");
  }
  return s;
}

async function main() {
  const [, , method, ...rest] = process.argv;
  if (!method) {
    console.error("Usage: node test_api.js <method> [k=v ...]");
    process.exit(2);
  }

  const params = parseArgs(rest);
  params.token = params.token || TOKEN;

  const qs = new URLSearchParams(params).toString();
  const path = `/api/${method}?${qs}`;

  const req = http.request(
    {
      host: HOST,
      port: PORT,
      method: "GET",
      path,
    },
    async (res) => {
      const body = await readAll(res);

      const code = int64LE(body);
      const payload = body.length >= 8 ? body.slice(8) : Buffer.alloc(0);

      console.log("REQUEST:", `http://${HOST}:${PORT}${path}`);
      console.log("httpStatus:", res.statusCode);
      console.log("contentLengthHeader:", res.headers["content-length"] || "");
      console.log("total_len:", body.length);
      console.log("code(int64):", code);
      console.log("payload_len:", payload.length);
      console.log("payload_bytes:", Array.from(payload));
      console.log("payload_repr:", bufRepr(payload));

      // try utf8 text (safe)
      const text = payload.toString("utf8");
      console.log("payload_text_utf8:");
      console.log(text);
    }
  );

  req.on("error", (e) => {
    console.error("request error:", e.message);
    process.exit(1);
  });

  req.end();
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
