const http = require("http");
const { URL } = require("url");
const Database = require("better-sqlite3");

const DEBUG = process.env.DEBUG === "1";
function dlog(...args) {
  if (DEBUG) console.log(...args);
}

const TOKEN = process.env.TOKEN || "TODO";

function checkToken(q) {
  const t = q.get("token") || "";
  return t === TOKEN;
}

// Binary protocol: [int64 return_code LE][payload bytes...]
function makeResponse(code, payloadBuf) {
  const codeBuf = Buffer.alloc(8);
  codeBuf.writeBigInt64LE(BigInt(code), 0);
  return Buffer.concat([codeBuf, payloadBuf || Buffer.alloc(0)]);
}

function sendOk(res, code, payload) {
  const payloadBuf = Buffer.isBuffer(payload)
    ? payload
    : Buffer.from(payload || "", "utf8");

  const out = makeResponse(code, payloadBuf);
  res.writeHead(200, {
    "Content-Type": "application/octet-stream",
    "Content-Length": String(out.length),
    "Connection": "close",
  });
  res.end(out);
}

// Simple persistent storage (SQLite)
// Use local file DB so it persists across reboots
const path = require("path");
const db = new Database(path.join(__dirname, "vtfs.db"));

// Linux mode bits (octal)
const S_IFDIR = 0o040000;
const S_IFREG = 0o100000;
const PERM777 = 0o777;

// type: 1=DIR, 2=FILE (match your vtfs_node_type)
const TYPE_DIR = 1;
const TYPE_FILE = 2;

function initDb() {
  db.exec(`
    CREATE TABLE IF NOT EXISTS nodes (
      ino   INTEGER PRIMARY KEY,
      type  INTEGER NOT NULL,
      mode  INTEGER NOT NULL,
      nlink INTEGER NOT NULL,
      data  BLOB
    );
    CREATE TABLE IF NOT EXISTS children (
      parent_ino INTEGER NOT NULL,
      name       TEXT NOT NULL,
      child_ino  INTEGER NOT NULL,
      UNIQUE(parent_ino, name)
    );
  `);

  const insNode = db.prepare(
    "INSERT OR IGNORE INTO nodes(ino,type,mode,nlink,data) VALUES(?,?,?,?,?)"
  );
  const insChild = db.prepare(
    "INSERT OR IGNORE INTO children(parent_ino,name,child_ino) VALUES(?,?,?)"
  );

  insNode.run(1000, TYPE_DIR, (S_IFDIR | PERM777), 1, null);
  insNode.run(200,  TYPE_DIR, (S_IFDIR | PERM777), 1, null);
  insChild.run(1000, "dir", 200);
}

initDb();

// API methods
function api_ping(_q) {
  dlog(`[PING]`);
  return { code: 0, payload: "pong" };
}

// list: returns lines: "<name>\t<ino>\t<type>\n"
function api_list(q) {
  const parent = Number(q.get("parent"));
  dlog(`[LIST] parent=${parent}`);
  if (!Number.isFinite(parent)) return { code: 100, payload: "bad_parent\n" };

  const rows = db
    .prepare(`
      SELECT c.name AS name, n.ino AS ino, n.type AS type
      FROM children c
      JOIN nodes n ON n.ino = c.child_ino
      WHERE c.parent_ino = ?
      ORDER BY c.name ASC
    `)
    .all(parent);

  let out = "";
  for (const r of rows) {
    out += `${r.name}\t${r.ino}\t${r.type}\n`;
  }
  return { code: 0, payload: out };
}

// lookup: returns one line: "<ino> <type> <mode> <nlink>\n"
function api_lookup(q) {
  const parent = Number(q.get("parent"));
  const name = q.get("name");
  dlog(`[LOOKUP] parent=${parent} name=${name || ""}`);
  if (!Number.isFinite(parent) || !name) return { code: 101, payload: "bad_args\n" };

  const row = db
    .prepare(`
      SELECT n.ino AS ino, n.type AS type, n.mode AS mode, n.nlink AS nlink
      FROM children c
      JOIN nodes n ON n.ino = c.child_ino
      WHERE c.parent_ino = ? AND c.name = ?
    `)
    .get(parent, name);

  if (!row) return { code: 2, payload: "" }; // "not found" (code > 0)
  return { code: 0, payload: `${row.ino} ${row.type} ${row.mode} ${row.nlink}\n` };
}

// HTTP server 
const server = http.createServer((req, res) => {
  let result = { code: 0, payload: "" };

  try {
    const u = new URL(req.url, "http://127.0.0.1:8080");
    const q = u.searchParams;

    dlog(`[REQ] ${req.method} ${u.pathname}${u.search}`);

    const parts = u.pathname.split("/").filter(Boolean); // ["api", "<method>"]
    const method = parts[0] === "api" ? parts[1] : null;

    if (DEBUG) {
      const entries = [];
      for (const [k, v] of q.entries()) entries.push(`${k}=${v}`);
      dlog(`[API] method=${method || ""}`);
      dlog(`[QS] ${entries.join("&")}`);
    }

    if (req.method !== "GET" || !method) {
      result = { code: 1, payload: "bad_request\n" };
      dlog(`[RES] code=${result.code} payload_len=${Buffer.byteLength(result.payload, "utf8")}`);
      return sendOk(res, result.code, result.payload);
    }

    if (!checkToken(q)) {
      result = { code: 4, payload: "bad_token\n" };
      dlog(`[RES] code=${result.code} payload_len=${Buffer.byteLength(result.payload, "utf8")}`);
      return sendOk(res, result.code, result.payload);
    }

    if (method === "ping") result = api_ping(q);
    else if (method === "list") result = api_list(q);
    else if (method === "lookup") result = api_lookup(q);
    else result = { code: 2, payload: "unknown_method\n" };

    dlog(`[RES] code=${result.code} payload_len=${Buffer.byteLength(result.payload || "", "utf8")}`);
    return sendOk(res, result.code, result.payload);
  } catch (e) {
    if (DEBUG) console.log("[ERR]", e && e.stack ? e.stack : String(e));
    result = { code: 3, payload: "server_error\n" };
    dlog(`[RES] code=${result.code} payload_len=${Buffer.byteLength(result.payload, "utf8")}`);
    return sendOk(res, result.code, result.payload);
  }
});

server.listen(8080, "0.0.0.0", () => {
  console.log("VTFS server listening on 0.0.0.0:8080");
});
