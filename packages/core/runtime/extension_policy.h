/* What BDK decides about an extension before Chromium sees it: whether its source is allowed
   (T-EXT-05) and whether its manifest can work in this product (T-EXT-06). extensions.md sections
   5 and 6; configuration.md 3.7. Platform-neutral C++23 with no Chromium, so the stub and the
   engine make the same decisions and the conformance suite tests them on both. */
#ifndef BDK_EXTENSION_POLICY_H_
#define BDK_EXTENSION_POLICY_H_

#include <string>
#include <vector>

#include "core_internal.h"

namespace bdkcore::ext {

enum class Source : uint8_t { kUnpacked, kCrx, kUpdateUrl, kStore, kBundled };

/* The ExtensionSourceKind names of interface/extensions.json. */
bool parse_source(const std::string& kind, Source* out);
const char* source_name(Source);

/* The named stores BDK ships an adapter for (configuration.md 3.7, decision D4). */
bool store_known(const std::string& store);

/* Why the configuration refuses a source, naming the bdk.toml key, or empty when it is allowed
   (extension.sourceNotAllowed). update_url is checked for kUpdateUrl, store for kStore. Unpacked
   directories also need developer_mode other than "off". A bundled extension is part of the
   signed application and always allowed. */
std::string source_refusal(const ExtensionConfig&, Source, const std::string& update_url,
                           const std::string& store);

/* Whether [extensions.install] blocklist names the identifier: it never installs or loads, from
   any source, bundled ones included, whatever install_policy says (extension.blocklisted). */
bool blocklisted(const ExtensionConfig&, const std::string& id);

/* The extension.blocklisted message for the identifier. */
std::string blocklist_refusal(const std::string& id);

/* The directory of a bundled extension: path under the bundle's Contents/Resources. */
std::string bundled_directory(const Runtime&, const ExtensionConfig::Bundled&);

/* An update_urls pattern: `*` matches any run of characters, everything else matches itself, and
   the whole URL must match. */
bool url_pattern_matches(const std::string& pattern, const std::string& url);

/* What the engine loads, for the checks that depend on it. */
struct EngineFacts {
    int chrome_major = 0;               /* the Chromium milestone of the engine */
    std::vector<int> manifest_versions; /* the manifest versions it loads */
};

/* One finding of a compatibility report (CompatibilityFinding). */
struct Finding {
    std::string severity; /* blocking, degrading, informational */
    std::string code;     /* surface.sidePanel.disabled, api.identity.getAuthToken.unavailable */
    std::string subject;  /* the manifest key or permission */
    std::string message;  /* names the cause and, where there is one, the bdk.toml key */
};

struct Report {
    std::vector<Finding> findings;
    /* compatible, degraded or incompatible. */
    std::string verdict() const;
    const Finding* first_blocking() const;
    /* The CompatibilityReport of interface/extensions.json. */
    std::string json() const;
};

/* Inspects a manifest (the text of manifest.json) before installing it or applying an update
   (extensions.md section 6): manifest version, minimum_chrome_version, permissions for APIs this
   product disables or lacks, surfaces the configuration disables, and update_url against
   update_urls. Never fails: text that is not a manifest is one blocking manifest.invalid finding.
   What inspection cannot see (optional permissions requested later, APIs chosen at runtime) fails
   when it happens instead. */
Report inspect_manifest(const Runtime&, const EngineFacts&, const std::string& manifest_json);

/* The ExtensionSurface names the manifest uses, in the enum's order. */
std::vector<std::string> manifest_surfaces(const std::string& manifest_json);

/* Whether an install or update is refused: a blocking finding refuses it unless install_policy is
   allow_all, which shows the findings in the install prompt instead. message is then the
   extension.incompatible message, naming the first blocking finding. */
bool install_refused(const ExtensionConfig&, const Report&, std::string* message);

/* The text the store's install button gets when a store source is enabled (decision D4). */
std::string store_install_label(const std::string& browser_name);

} // namespace bdkcore::ext

#endif /* BDK_EXTENSION_POLICY_H_ */
