import Link from "next/link";

export default function HomePage() {
  return (
    <main className="mx-auto flex max-w-2xl flex-1 flex-col justify-center gap-4 px-4 py-16">
      <h1 className="text-3xl font-bold">BDK, the Browser Development Kit</h1>
      <p>
        Build a Chromium-based browser for macOS in Swift. BDK runs the engine; your application
        owns every window, menu and control, and replaces Chromium&apos;s own pages and prompts
        where it wants.
      </p>
      <p className="flex gap-4">
        <Link href="/docs/getting-started/" className="font-medium underline">
          Get started
        </Link>
        <Link href="/docs/" className="font-medium underline">
          Read the documentation
        </Link>
      </p>
    </main>
  );
}
