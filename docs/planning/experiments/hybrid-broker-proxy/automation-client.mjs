// Stand-in for the independent trusted automation host: separate process, authenticated local IPC, no page involved.
import net from "node:net";
const [sock, token, ...cmds] = process.argv.slice(2);
const c = net.createConnection(sock);
let buf = ""; const out = [];
c.on("data", (d) => { buf += d; let i; while ((i = buf.indexOf("\n")) >= 0) { out.push(JSON.parse(buf.slice(0, i))); buf = buf.slice(i + 1); if (out.length === cmds.length + 1) { console.log(JSON.stringify(out)); c.end(); } } });
c.write(JSON.stringify({ hello: token }) + "\n");
for (const cmd of cmds) { const [command, json] = cmd.split("="); c.write(JSON.stringify({ command, args: JSON.parse(json ?? "{}") }) + "\n"); }
