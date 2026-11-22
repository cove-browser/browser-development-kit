// Fails: the task finishes with automation.scriptFailed and this message.
export default async function main() {
  throw new Error("boom");
}
