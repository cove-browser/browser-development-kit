import type { Metadata } from "next";
import type { ReactNode } from "react";
import { Provider } from "@/components/provider";
import "./global.css";

export const metadata: Metadata = {
  title: { template: "%s | BDK", default: "BDK, the Browser Development Kit" },
  description: "Build a Chromium-based browser for macOS in Swift.",
};

// The system font: the build fetches nothing from the network.
export default function Layout({ children }: { children: ReactNode }) {
  return (
    <html lang="en" suppressHydrationWarning>
      <body className="flex min-h-screen flex-col">
        <Provider>{children}</Provider>
      </body>
    </html>
  );
}
