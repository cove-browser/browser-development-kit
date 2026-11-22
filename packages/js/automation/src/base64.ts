const ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * Decodes standard base64. Written out because the automation host has no `atob` or `Buffer`.
 */
export function decodeBase64(text: string): Uint8Array {
  const clean = text.replace(/=+$/, "");
  const out = new Uint8Array(Math.floor((clean.length * 3) / 4));
  let bits = 0;
  let value = 0;
  let at = 0;
  for (const char of clean) {
    const index = ALPHABET.indexOf(char);
    if (index < 0) throw new SyntaxError(`invalid base64 character ${JSON.stringify(char)}`);
    value = (value << 6) | index;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[at++] = (value >> bits) & 0xff;
    }
  }
  return out;
}
