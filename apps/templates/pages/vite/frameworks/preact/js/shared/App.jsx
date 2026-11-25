// Shared by every page at build time. Preact builds the DOM without HTML strings, which the content
// security policy (Trusted Types) forbids; do not use dangerouslySetInnerHTML.
import { useEffect, useState } from "preact/hooks";

export function App({ title, load }) {
  const [items, setItems] = useState(undefined);
  const [error, setError] = useState(undefined);
  useEffect(() => {
    load().then(setItems, (e) => setError(e instanceof Error ? e.message : String(e)));
  }, [load]);
  return (
    <main class="mx-auto max-w-2xl p-4">
      <h1 class="text-2xl font-semibold">{title}</h1>
      <p role="status" class={error ? "error text-red-600" : "muted opacity-70"}>
        {error
          ? `Could not load: ${error}`
          : items === undefined
            ? "Loading"
            : items.length === 0
              ? "Nothing here yet."
              : ""}
      </p>
      <ul>
        {(items ?? []).map((item, index) => (
          <li key={index} class="flex flex-col border-b py-2">
            {item.href ? <a href={item.href}>{item.label}</a> : <span>{item.label}</span>}
            <small class="opacity-70">{item.detail}</small>
          </li>
        ))}
      </ul>
    </main>
  );
}
