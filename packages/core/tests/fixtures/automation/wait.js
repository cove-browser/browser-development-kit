// Runs until it is cancelled or its session is revoked.
export default async function main() {
  setTimeout(() => console.log("still waiting"), 0);
  await new Promise(() => {});
}
