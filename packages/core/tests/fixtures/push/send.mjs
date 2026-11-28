// A Web Push test sender for the engine scenario engine_web_apps_a_push_from_google_reaches_the_app
// (T-PWA-05). It uses VAPID (RFC 8292) and sends no payload, so nothing needs encrypting.
//   node send.mjs keys                   prints the public key, then the private key (JWK, base64url)
//   node send.mjs send <endpoint> <jwk>  sends one push and prints the push service's status
import { createPrivateKey, generateKeyPairSync, sign } from "node:crypto";

const b64url = (data) => Buffer.from(data).toString("base64url");

function publicKeyOf(jwk) {
  return b64url(
    Buffer.concat([
      Buffer.from([4]),
      Buffer.from(jwk.x, "base64url"),
      Buffer.from(jwk.y, "base64url"),
    ]),
  );
}

const [mode, endpoint, encoded] = process.argv.slice(2);
if (mode === "keys") {
  const { privateKey } = generateKeyPairSync("ec", { namedCurve: "prime256v1" });
  const jwk = privateKey.export({ format: "jwk" });
  console.log(publicKeyOf(jwk));
  console.log(b64url(JSON.stringify(jwk)));
} else if (mode === "send" && endpoint && encoded) {
  const jwk = JSON.parse(Buffer.from(encoded, "base64url").toString());
  const header = b64url(JSON.stringify({ typ: "JWT", alg: "ES256" }));
  const claims = b64url(
    JSON.stringify({
      aud: new URL(endpoint).origin,
      exp: Math.floor(Date.now() / 1000) + 3600,
      sub: "mailto:push-test@bdk.invalid",
    }),
  );
  const signature = sign("sha256", Buffer.from(`${header}.${claims}`), {
    key: createPrivateKey({ key: jwk, format: "jwk" }),
    dsaEncoding: "ieee-p1363",
  });
  const response = await fetch(endpoint, {
    method: "POST",
    headers: {
      Authorization: `vapid t=${header}.${claims}.${b64url(signature)}, k=${publicKeyOf(jwk)}`,
      TTL: "60",
      Urgency: "high",
    },
  });
  console.log(response.status);
} else {
  console.error("usage: send.mjs keys | send <endpoint> <jwk>");
  process.exit(2);
}
