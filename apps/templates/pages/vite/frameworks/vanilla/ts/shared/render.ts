// Shared by every page at build time. The content security policy forbids HTML strings (Trusted Types),
// so the DOM is built with createElement and textContent.
export interface Item {
  label: string;
  detail: string;
  href?: string;
}

export async function render(
  root: Element | null,
  title: string,
  load: () => Promise<Item[]>,
): Promise<void> {
  if (root === null) return;
  const main = document.createElement("main");
  main.className = "mx-auto max-w-2xl p-4";
  const heading = document.createElement("h1");
  heading.className = "text-2xl font-semibold";
  heading.textContent = title;
  const status = document.createElement("p");
  status.className = "muted opacity-70";
  status.setAttribute("role", "status");
  status.textContent = "Loading";
  const list = document.createElement("ul");
  main.append(heading, status, list);
  root.append(main);
  try {
    const items = await load();
    status.textContent = items.length === 0 ? "Nothing here yet." : "";
    for (const item of items) {
      const row = document.createElement("li");
      row.className = "flex flex-col border-b py-2";
      const label = document.createElement(item.href ? "a" : "span");
      if (item.href && label instanceof HTMLAnchorElement) label.href = item.href;
      label.textContent = item.label;
      const detail = document.createElement("small");
      detail.className = "opacity-70";
      detail.textContent = item.detail;
      row.append(label, detail);
      list.append(row);
    }
  } catch (error) {
    status.textContent = `Could not load: ${error instanceof Error ? error.message : String(error)}`;
    status.classList.add("error", "text-red-600");
  }
}
