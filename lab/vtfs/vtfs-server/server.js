const http = require("http");
const { URL } = require("url");
const Database = require("better-sqlite3");

// Debugger
const DEBUG = process.env.DEBUG === "1";
function dlog(...args) {
  if (DEBUG) console.log(...args);
}

const TOKEN = process.env.TOKEN || "TODO";

// Helpers
function checkToken(q) {
  const t = q.get("token") || "";
  return t === TOKEN;
}

function b64urlDecode(s) {
  s = (s || "").replace(/-/g, "+").replace(/_/g, "/");
  while (s.length % 4) s += "=";
  return Buffer.from(s, "base64");
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

function allocIno() {
  const row = db.prepare("SELECT COALESCE(MAX(ino), 999) AS m FROM nodes").get();
  return Number(row.m) + 1;
}

// API methods
// ping: return pong
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

// create: return <ino> <type> <mode> <nlink>\n
function api_create(q) {
  const parent = Number(q.get("parent"));
  const name = q.get("name");
  const mode = Number(q.get("mode")); // decimal
  if (!Number.isInteger(parent) || !name) return { code: 101, payload: "bad_args\n" };

  const exists = db.prepare("SELECT 1 FROM children WHERE parent_ino=? AND name=?")
                  .get(parent, name);
  if (exists) return { code: 17, payload: "exist\n" };

  const prow = db.prepare("SELECT type FROM nodes WHERE ino=?").get(parent);
  if (!prow) return { code: 2, payload: "" };            // ENOENT
  if (prow.type !== TYPE_DIR) return { code: 20, payload:"notdir\n" }; // ENOTDIR=20

  const ino = allocIno();
  const type = TYPE_FILE;
  const m = (mode && Number.isInteger(mode)) ? mode : (S_IFREG | PERM777);

  const tx = db.transaction(() => {
    db.prepare("INSERT INTO nodes(ino,type,mode,nlink,data) VALUES(?,?,?,?,?)")
      .run(ino, type, m, 1, Buffer.alloc(0));
    db.prepare("INSERT INTO children(parent_ino,name,child_ino) VALUES(?,?,?)")
      .run(parent, name, ino);
  });
  tx();

  return { code: 0, payload: `${ino} ${type} ${m} 1\n` };
}

// mkdir: return  code=0, payload=""
function api_mkdir(q) {
  const parent = Number(q.get("parent"));
  const name = q.get("name");
  const mode = Number(q.get("mode"));
  if (!Number.isInteger(parent) || !name) return { code: 101, payload: "bad_args\n" };

  const exists = db.prepare("SELECT 1 FROM children WHERE parent_ino=? AND name=?")
                  .get(parent, name);
  if (exists) return { code: 17, payload: "exist\n" };

  const prow = db.prepare("SELECT type FROM nodes WHERE ino=?").get(parent);
  if (!prow) return { code: 2, payload: "" };            // ENOENT
  if (prow.type !== TYPE_DIR) return { code: 20, payload:"notdir\n" }; // ENOTDIR=20

  const ino = allocIno();
  const type = TYPE_DIR;
  const m = (mode && Number.isInteger(mode)) ? mode : (S_IFDIR | PERM777);

  const tx = db.transaction(() => {
    db.prepare("INSERT INTO nodes(ino,type,mode,nlink,data) VALUES(?,?,?,?,?)")
      .run(ino, type, m, 1, null);
    db.prepare("INSERT INTO children(parent_ino,name,child_ino) VALUES(?,?,?)")
      .run(parent, name, ino);
  });
  tx();

  return { code: 0, payload: `${ino} ${type} ${m} 1\n` };
}

// unlink: return  code=0, payload=""
function api_unlink(q) {
  const parent = Number(q.get("parent"));
  const name = q.get("name");
  if (!Number.isInteger(parent) || !name) return { code: 101, payload: "bad_args\n" };

  const row = db.prepare(`
      SELECT n.ino AS ino, n.type AS type, n.nlink AS nlink
      FROM children c JOIN nodes n ON n.ino=c.child_ino
      WHERE c.parent_ino=? AND c.name=?
  `).get(parent, name);

  if (!row) return { code: 2, payload: "" };
  if (row.type !== TYPE_FILE) return { code: 21, payload: "isdir\n" };

  const tx = db.transaction(() => {
    db.prepare("DELETE FROM children WHERE parent_ino=? AND name=?").run(parent, name);
    db.prepare("UPDATE nodes SET nlink=nlink-1 WHERE ino=?").run(row.ino);

    const left = db.prepare("SELECT nlink FROM nodes WHERE ino=?").get(row.ino).nlink;
    if (left <= 0) db.prepare("DELETE FROM nodes WHERE ino=?").run(row.ino);
  });
  tx();

  return { code: 0, payload: "" };
}

// rmdir: return  code=0, payload=""
function api_rmdir(q) {
  const parent = Number(q.get("parent"));
  const name = q.get("name");
  if (!Number.isInteger(parent) || !name) return { code: 101, payload: "bad_args\n" };

  const row = db.prepare(`
      SELECT n.ino AS ino, n.type AS type
      FROM children c JOIN nodes n ON n.ino=c.child_ino
      WHERE c.parent_ino=? AND c.name=?
  `).get(parent, name);

  if (!row) return { code: 2, payload: "" };
  if (row.type !== TYPE_DIR) return { code: 20, payload: "notdir\n" };

  const cnt = db.prepare("SELECT COUNT(*) AS c FROM children WHERE parent_ino=?")
                .get(row.ino).c;
  if (cnt > 0) return { code: 39, payload: "notempty\n" };

  const tx = db.transaction(() => {
    db.prepare("DELETE FROM children WHERE parent_ino=? AND name=?").run(parent, name);
    db.prepare("DELETE FROM nodes WHERE ino=?").run(row.ino);
  });
  tx();

  return { code: 0, payload: "" };
}

// read: return bytes
function api_read(q) {
  const ino = Number(q.get("ino"));
  const off = Number(q.get("off") || 0);
  const len = Number(q.get("len") || 0);
  if (!Number.isInteger(ino) || off < 0 || len < 0) return { code: 101, payload: "bad_args\n" };

  const row = db.prepare("SELECT type, data FROM nodes WHERE ino=?").get(ino);
  if (!row) return { code: 2, payload: "" };
  if (row.type !== TYPE_FILE) return { code: 21, payload: "isdir\n" };

  const data = row.data ? Buffer.from(row.data) : Buffer.alloc(0);
  const slice = data.subarray(off, Math.min(off + len, data.length));
  const hdr = Buffer.alloc(8);
  hdr.writeBigInt64LE(BigInt(slice.length), 0);
  return { code: 0, payload: Buffer.concat([hdr, slice]) };
}

// write: write bytes
function api_write(q) {
  const ino = Number(q.get("ino"));
  const off = Number(q.get("off") || 0);
  const dataEnc = q.get("data") || "";
  if (!Number.isInteger(ino) || off < 0) return { code: 101, payload: "bad_args\n" };

  const row = db.prepare("SELECT type, data FROM nodes WHERE ino=?").get(ino);
  if (!row) return { code: 2, payload: "" };
  if (row.type !== TYPE_FILE) return { code: 21, payload: "isdir\n" };

  const chunk = b64urlDecode(dataEnc);
  const old = row.data ? Buffer.from(row.data) : Buffer.alloc(0);

  const need = off + chunk.length;
  const out = Buffer.alloc(Math.max(old.length, need));
  old.copy(out, 0, 0, old.length);
  chunk.copy(out, off);

  db.prepare("UPDATE nodes SET data=? WHERE ino=?").run(out, ino);
  return { code: 0, payload: "" };
}

// truncate: 
function api_truncate(q) {
  const ino = Number(q.get("ino"));
  if (!Number.isInteger(ino)) return { code: 101, payload: "bad_args\n" };

  const row = db.prepare("SELECT type FROM nodes WHERE ino=?").get(ino);
  if (!row) return { code: 2, payload: "" };
  if (row.type !== TYPE_FILE) return { code: 21, payload: "isdir\n" };

  db.prepare("UPDATE nodes SET data=? WHERE ino=?").run(Buffer.alloc(0), ino);
  return { code: 0, payload: "" };
}

// link: create hardlink (only for regular files)
function api_link(q) {
  const old_ino = Number(q.get("old_ino"));
  const parent = Number(q.get("parent"));
  const name = q.get("name");

  if (!Number.isInteger(old_ino) || !Number.isInteger(parent) || !name)
    return { code: 101, payload: "bad_args\n" };

  const old = db.prepare("SELECT type, mode, nlink FROM nodes WHERE ino=?").get(old_ino);
  if (!old) return { code: 2, payload: "" };
  if (old.type !== TYPE_FILE) return { code: 21, payload: "isdir\n" }; // forbid dir

  const exists = db.prepare("SELECT 1 FROM children WHERE parent_ino=? AND name=?")
                   .get(parent, name);
  if (exists) return { code: 17, payload: "exist\n" };

  const tx = db.transaction(() => {
    db.prepare("INSERT INTO children(parent_ino,name,child_ino) VALUES(?,?,?)")
      .run(parent, name, old_ino);
    db.prepare("UPDATE nodes SET nlink=nlink+1 WHERE ino=?").run(old_ino);
  });
  tx();

  // return updated info
  const row = db.prepare("SELECT ino, type, mode, nlink FROM nodes WHERE ino=?").get(old_ino);
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

    // hook methods into router
    if (method === "ping") result = api_ping(q);
    else if (method === "list") result = api_list(q);
    else if (method === "lookup") result = api_lookup(q);
    else if (method === "create") result = api_create(q);
    else if (method === "mkdir")  result = api_mkdir(q);
    else if (method === "unlink") result = api_unlink(q);
    else if (method === "rmdir")  result = api_rmdir(q);
    else if (method === "read") result = api_read(q);
    else if (method === "write") result = api_write(q);
    else if (method === "truncate") result = api_truncate(q);
    else if (method === "link") result = api_link(q);
    else result = { code: 1, payload: "unknown_method\n" };

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
