// Uses CPU until the host stops it at its task's budget: in one run that never returns ("spin"), or
// in short slices between timers, so only the sum of the slices reaches the budget ("slices").
function burn(ms) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    // Busy.
  }
}

export default async function main(args) {
  if (args.mode === "spin") {
    for (;;) burn(1000);
  }
  for (;;) {
    burn(50);
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
}
