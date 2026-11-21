// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/passwords_service.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/services/autofill_service.h"
#include "bdk/engine/services/events.h"
#include "bdk/engine/services/service_call.h"
#include "bdk/engine/services/service_common.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_store/password_store_change.h"
#include "components/password_manager/core/browser/password_store/password_store_consumer.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "components/password_manager/core/browser/password_store/stored_credential.h"
#include "components/password_manager/core/browser/password_string.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {

namespace {

using bdkcore::BuiltinCall;
using password_manager::PasswordForm;
using password_manager::PasswordStoreInterface;
using password_manager::StoredCredential;
using Credentials = std::vector<StoredCredential>;
using Store = scoped_refptr<PasswordStoreInterface>;

// Reads the whole store once and hands the result to `done`.
class LoginsReader : public password_manager::PasswordStoreConsumer {
 public:
  static void ReadAll(PasswordStoreInterface* store,
                      base::OnceCallback<void(Credentials)> done) {
    // Deletes itself when the store answers.
    auto* reader = new LoginsReader(std::move(done));
    store->GetAllLogins(reader->weak_factory_.GetWeakPtr());
  }

 private:
  explicit LoginsReader(base::OnceCallback<void(Credentials)> done)
      : done_(std::move(done)) {}
  ~LoginsReader() override = default;

  void OnGetPasswordStoreResultsOrErrorFrom(
      PasswordStoreInterface* store,
      base::expected<Credentials, password_manager::PasswordStoreBackendError>
          results) override {
    Credentials credentials =
        results.has_value() ? std::move(results).value() : Credentials();
    std::move(done_).Run(std::move(credentials));
    delete this;
  }

  base::OnceCallback<void(Credentials)> done_;
  base::WeakPtrFactory<LoginsReader> weak_factory_{this};
};

std::string IdOf(const StoredCredential& credential) {
  return credential.primary_key
             ? base::NumberToString(credential.primary_key->value())
             : std::string();
}

std::string OriginOf(const StoredCredential& credential) {
  const url::Origin origin = url::Origin::Create(credential.url);
  return origin.opaque() ? credential.signon_realm : origin.GetURL().spec();
}

// Written as text so the times are whole milliseconds, as JsonTime writes
// them; a double in a DictValue would keep a fraction or gain a ".0".
std::string InfoJson(const StoredCredential& credential) {
  return "{\"id\":" + JsonString(IdOf(credential)) +
         ",\"origin\":" + JsonString(OriginOf(credential)) + ",\"username\":" +
         JsonString(base::UTF16ToUTF8(credential.username_value)) +
         ",\"created\":" + JsonTime(credential.date_created) +
         ",\"lastUsed\":" +
         (credential.date_last_used.is_null()
              ? std::string("null")
              : JsonTime(credential.date_last_used)) +
         "}";
}

bool SameKey(const StoredCredential& a, const StoredCredential& b) {
  return password_manager::AreStoredCredentialUniqueKeysEqual(a, b);
}

const StoredCredential* FindById(const Credentials& all,
                                 const std::string& id) {
  for (const StoredCredential& credential : all) {
    if (!credential.blocked_by_user && IdOf(credential) == id) {
      return &credential;
    }
  }
  return nullptr;
}

// A credential holding only what identifies another one in the store.
StoredCredential KeyOf(const StoredCredential& credential) {
  StoredCredential key;
  key.primary_key = credential.primary_key;
  key.signon_realm = credential.signon_realm;
  key.url = credential.url;
  key.username_element = credential.username_element;
  key.username_value = credential.username_value;
  key.password_element = credential.password_element;
  return key;
}

void RejectNoLogin(const BuiltinCall& call, const std::string& id) {
  RejectCall(call, BDK_CODE_INVALID_ARGUMENTS, "no saved login '" + id + "'");
}

// ---- Commands ------------------------------------------------------------

void List(const std::shared_ptr<BuiltinCall>& call, const Store& store) {
  const bool for_origin = call->name == "passwords.forOrigin";
  url::Origin wanted;
  if (for_origin) {
    wanted =
        url::Origin::Create(GURL(*ArgumentsOf(*call).FindString("origin")));
  }
  LoginsReader::ReadAll(
      store.get(),
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call, bool for_origin,
             url::Origin wanted, Store, Credentials all) {
            std::string items;
            for (const StoredCredential& credential : all) {
              if (credential.blocked_by_user) {
                continue;
              }
              if (for_origin && url::Origin::Create(credential.url) != wanted) {
                continue;
              }
              items += (items.empty() ? "" : ",") + InfoJson(credential);
            }
            call->control->Resolve("{\"items\":[" + items + "]}");
          },
          call, for_origin, wanted, store));
}

void Reveal(const std::shared_ptr<BuiltinCall>& call, const Store& store) {
  const std::string id = *ArgumentsOf(*call).FindString("id");
  LoginsReader::ReadAll(
      store.get(),
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call, std::string id, Store,
             Credentials all) {
            const StoredCredential* found = FindById(all, id);
            if (!found) {
              RejectNoLogin(*call, id);
              return;
            }
            call->control->Resolve(ToJson(base::DictValue().Set(
                "password", base::UTF16ToUTF8(found->password_value.value()))));
          },
          call, id, store));
}

// Answers with the login `like` as the store holds it now.
void ResolveWithInfoOf(const std::shared_ptr<BuiltinCall>& call,
                       const Store& store,
                       StoredCredential like) {
  LoginsReader::ReadAll(
      store.get(),
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call, StoredCredential like, Store,
             Credentials all) {
            for (const StoredCredential& credential : all) {
              if (!credential.blocked_by_user && SameKey(credential, like)) {
                call->control->Resolve(InfoJson(credential));
                return;
              }
            }
            RejectCall(*call, "failed", "the login was not stored");
          },
          call, std::move(like), store));
}

void Add(const std::shared_ptr<BuiltinCall>& call, const Store& store) {
  const base::DictValue args = ArgumentsOf(*call);
  const GURL url(*args.FindString("origin"));
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    RejectCall(*call, BDK_CODE_INVALID_ARGUMENTS,
               "origin is not an http or https URL");
    return;
  }
  const url::Origin origin = url::Origin::Create(url);
  StoredCredential credential;
  credential.scheme = PasswordForm::Scheme::kHtml;
  credential.signon_realm = origin.GetURL().spec();
  credential.url = origin.GetURL();
  credential.username_value = base::UTF8ToUTF16(*args.FindString("username"));
  credential.password_value = password_manager::PasswordString(
      base::UTF8ToUTF16(*args.FindString("password")));
  credential.date_created = base::Time::Now();
  credential.date_password_modified = credential.date_created;
  credential.type = PasswordForm::Type::kManuallyAdded;
  credential.in_store = PasswordForm::Store::kProfileStore;
  StoredCredential key = KeyOf(credential);
  store->AddLogin(
      std::move(credential),
      base::BindOnce(&ResolveWithInfoOf, call, store, std::move(key)));
}

void Update(const std::shared_ptr<BuiltinCall>& call, const Store& store) {
  const std::string id = *ArgumentsOf(*call).FindString("id");
  LoginsReader::ReadAll(
      store.get(),
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call, std::string id, Store store,
             Credentials all) {
            const base::DictValue args = ArgumentsOf(*call);
            auto it = std::ranges::find_if(all, [&](const StoredCredential& c) {
              return !c.blocked_by_user && IdOf(c) == id;
            });
            if (it == all.end()) {
              RejectNoLogin(*call, id);
              return;
            }
            StoredCredential old_key = KeyOf(*it);
            StoredCredential updated = std::move(*it);
            if (const std::string* username = args.FindString("username")) {
              updated.username_value = base::UTF8ToUTF16(*username);
            }
            if (const std::string* password = args.FindString("password")) {
              updated.password_value = password_manager::PasswordString(
                  base::UTF8ToUTF16(*password));
              updated.date_password_modified = base::Time::Now();
            }
            StoredCredential key = KeyOf(updated);
            // Only a change of the key (the username) makes the store replace
            // the login and give it a new identifier; a new password updates it
            // in place.
            if (SameKey(updated, old_key)) {
              store->UpdateLogin(std::move(updated),
                                 base::BindOnce(&ResolveWithInfoOf, call, store,
                                                std::move(key)));
              return;
            }
            store->UpdateLoginWithPrimaryKey(
                std::move(updated), old_key,
                base::BindOnce(&ResolveWithInfoOf, call, store,
                               std::move(key)));
          },
          call, id, store));
}

void Remove(const std::shared_ptr<BuiltinCall>& call, const Store& store) {
  const std::string id = *ArgumentsOf(*call).FindString("id");
  LoginsReader::ReadAll(
      store.get(),
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call, std::string id, Store store,
             Credentials all) {
            const StoredCredential* found = FindById(all, id);
            if (!found) {
              RejectNoLogin(*call, id);
              return;
            }
            store->RemoveLogin(FROM_HERE, *found);
            // The store runs its operations in order, so a read now sees it
            // gone.
            LoginsReader::ReadAll(
                store.get(),
                base::BindOnce(
                    [](std::shared_ptr<BuiltinCall> call, Store, Credentials) {
                      call->control->Resolve("null");
                    },
                    call, store));
          },
          call, id, store));
}

void NeverSaved(const std::shared_ptr<BuiltinCall>& call, const Store& store) {
  LoginsReader::ReadAll(
      store.get(),
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call, Store, Credentials all) {
            base::ListValue origins;
            for (const StoredCredential& credential : all) {
              if (credential.blocked_by_user) {
                origins.Append(OriginOf(credential));
              }
            }
            call->control->Resolve(
                ToJson(base::DictValue().Set("origins", std::move(origins))));
          },
          call, store));
}

void RemoveNeverSaved(const std::shared_ptr<BuiltinCall>& call,
                      const Store& store) {
  const url::Origin wanted =
      url::Origin::Create(GURL(*ArgumentsOf(*call).FindString("origin")));
  LoginsReader::ReadAll(
      store.get(),
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call, url::Origin wanted, Store store,
             Credentials all) {
            for (const StoredCredential& credential : all) {
              if (credential.blocked_by_user &&
                  url::Origin::Create(credential.url) == wanted) {
                store->RemoveLogin(FROM_HERE, credential);
              }
            }
            LoginsReader::ReadAll(
                store.get(),
                base::BindOnce(
                    [](std::shared_ptr<BuiltinCall> call, Store, Credentials) {
                      call->control->Resolve("null");
                    },
                    call, store));
          },
          call, wanted, store));
}

// ---- CSV (export and import) ---------------------------------------------

std::string CsvField(const std::string& value) {
  if (value.find_first_of(",\"\r\n") == std::string::npos) {
    return value;
  }
  std::string quoted = "\"";
  for (char c : value) {
    if (c == '"') {
      quoted += '"';
    }
    quoted += c;
  }
  return quoted + "\"";
}

// Rows of fields, with quoted fields and doubled quotes as RFC 4180 has them.
std::vector<std::vector<std::string>> ParseCsv(const std::string& text) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false;
  bool any = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quoted) {
      if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') {
        field += '"';
        ++i;
      } else if (c == '"') {
        quoted = false;
      } else {
        field += c;
      }
      continue;
    }
    if (c == '"') {
      quoted = true;
      any = true;
    } else if (c == ',') {
      row.push_back(std::move(field));
      field.clear();
      any = true;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
      if (any || !field.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
      }
      field.clear();
      row.clear();
      any = false;
    } else {
      field += c;
      any = true;
    }
  }
  if (any || !field.empty()) {
    row.push_back(std::move(field));
    rows.push_back(std::move(row));
  }
  return rows;
}

void Export(const std::shared_ptr<BuiltinCall>& call, const Store& store) {
  const base::FilePath path(*ArgumentsOf(*call).FindString("path"));
  LoginsReader::ReadAll(
      store.get(),
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call, base::FilePath path, Store,
             Credentials all) {
            std::string csv = "name,url,username,password,note\n";
            for (const StoredCredential& credential : all) {
              if (credential.blocked_by_user) {
                continue;
              }
              const std::string origin = OriginOf(credential);
              csv += CsvField(GURL(origin).GetHost()) + "," + CsvField(origin) +
                     "," +
                     CsvField(base::UTF16ToUTF8(credential.username_value)) +
                     "," +
                     CsvField(
                         base::UTF16ToUTF8(credential.password_value.value())) +
                     ",\n";
            }
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE, {base::MayBlock()},
                base::BindOnce(
                    [](base::FilePath path, std::string csv) {
                      return base::WriteFile(path, csv);
                    },
                    path, std::move(csv)),
                base::BindOnce(
                    [](std::shared_ptr<BuiltinCall> call, bool written) {
                      if (written) {
                        call->control->Resolve("null");
                      } else {
                        RejectCall(*call, "failed",
                                   "the file could not be written");
                      }
                    },
                    call));
          },
          call, path, store));
}

void Import(const std::shared_ptr<BuiltinCall>& call, const Store& store) {
  const base::FilePath path(*ArgumentsOf(*call).FindString("path"));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath path) -> std::optional<std::string> {
            std::string text;
            if (!base::ReadFileToString(path, &text)) {
              return std::nullopt;
            }
            return text;
          },
          path),
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call, Store store,
             std::optional<std::string> text) {
            // Cancelled while the file was read: the call is answered and
            // nothing it read is saved.
            if (call->control->cancelled()) {
              return;
            }
            if (!text) {
              RejectCall(*call, BDK_CODE_INVALID_ARGUMENTS,
                         "the file could not be read");
              return;
            }
            const auto rows = ParseCsv(*text);
            if (rows.empty()) {
              call->control->Resolve("{\"imported\":0}");
              return;
            }
            auto column = [&](std::string_view name) -> int {
              for (size_t i = 0; i < rows[0].size(); ++i) {
                if (base::EqualsCaseInsensitiveASCII(rows[0][i], name)) {
                  return static_cast<int>(i);
                }
              }
              return -1;
            };
            const int url_column = column("url");
            const int user_column = column("username");
            const int password_column = column("password");
            if (url_column < 0 || user_column < 0 || password_column < 0) {
              RejectCall(*call, BDK_CODE_INVALID_ARGUMENTS,
                         "the file needs url, username and password columns");
              return;
            }
            std::vector<StoredCredential> imported;
            for (size_t r = 1; r < rows.size(); ++r) {
              const auto& row = rows[r];
              if (row.size() <=
                  static_cast<size_t>(
                      std::max({url_column, user_column, password_column}))) {
                continue;
              }
              const GURL url(row[url_column]);
              if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
                continue;
              }
              const url::Origin origin = url::Origin::Create(url);
              StoredCredential credential;
              credential.scheme = PasswordForm::Scheme::kHtml;
              credential.signon_realm = origin.GetURL().spec();
              credential.url = origin.GetURL();
              credential.username_value = base::UTF8ToUTF16(row[user_column]);
              credential.password_value = password_manager::PasswordString(
                  base::UTF8ToUTF16(row[password_column]));
              credential.date_created = base::Time::Now();
              credential.date_password_modified = credential.date_created;
              credential.type = PasswordForm::Type::kImported;
              credential.in_store = PasswordForm::Store::kProfileStore;
              imported.push_back(std::move(credential));
            }
            const size_t count = imported.size();
            call->control->Progress(0.5, "importing");
            store->AddLogins(
                std::move(imported),
                base::BindOnce(
                    [](std::shared_ptr<BuiltinCall> call, Store, size_t count) {
                      call->control->Resolve(ToJson(base::DictValue().Set(
                          "imported", static_cast<int>(count))));
                    },
                    call, store, count));
          },
          call, store));
}

// Reports the changes to the profile's saved logins (passwords.changed) and,
// when a change touched the insecure-credential flags of a login, that the
// issues changed (passwords.issuesChanged). A login that is renamed is stored
// under a new key, so it is reported as a removal and an addition. Sites the
// user chose never to save are not logins and are not reported.
class PasswordEvents : public ServiceEvents,
                       public PasswordStoreInterface::Observer {
 public:
  PasswordEvents(Store store, std::weak_ptr<bdkcore::Profile> profile)
      : store_(std::move(store)), profile_(std::move(profile)) {
    store_->AddObserver(this);
  }

  ~PasswordEvents() override { store_->RemoveObserver(this); }

  void OnLoginsChanged(
      PasswordStoreInterface*,
      const password_manager::PasswordStoreChangeList& changes) override {
    bool issues_changed = false;
    for (const password_manager::PasswordStoreChange& change : changes) {
      issues_changed |= change.insecure_credentials_changed().value();
      if (change.credential().blocked_by_user) {
        continue;
      }
      const char* kind = "updated";
      if (change.type() == password_manager::PasswordStoreChange::ADD) {
        kind = "added";
      } else if (change.type() ==
                 password_manager::PasswordStoreChange::REMOVE) {
        kind = "removed";
      }
      EmitEvent(profile_, "passwords.changed",
                "{\"id\":" + JsonString(IdOf(change.credential())) +
                    ",\"change\":\"" + kind + "\",\"source\":\"application\"}");
    }
    if (issues_changed) {
      EmitEvent(profile_, "passwords.issuesChanged", "{}");
    }
  }

  void OnLoginsRetained(PasswordStoreInterface*,
                        const std::vector<StoredCredential>&) override {}

 private:
  Store store_;
  std::weak_ptr<bdkcore::Profile> profile_;
};

}  // namespace

std::unique_ptr<ServiceEvents> CreatePasswordEvents(
    Profile* profile,
    std::weak_ptr<bdkcore::Profile> bdk_profile) {
  Store store = ProfilePasswordStoreFactory::GetForProfile(
      profile, ServiceAccessType::EXPLICIT_ACCESS);
  if (!store) {
    return nullptr;
  }
  return std::make_unique<PasswordEvents>(std::move(store),
                                          std::move(bdk_profile));
}

bool ExecutePasswordsOrAutofillCommand(
    bdkcore::Runtime& rt,
    const std::shared_ptr<BuiltinCall>& call) {
  if (ExecuteAutofillCommand(rt, call)) {
    return true;
  }
  const std::string& name = call->name;
  if (!name.starts_with("passwords.")) {
    return false;
  }
  // The leak check needs a provider (M6): with none, nothing is checked or
  // sent, there are no issues, and no flag to dismiss (services.md 5.9).
  if (name == "passwords.check") {
    call->control->Reject(BDK_DOMAIN_PROVIDER, "unavailable",
                          "no leak-check provider is configured");
    return true;
  }
  if (name == "passwords.issues") {
    call->control->Resolve("{\"items\":[]}");
    return true;
  }
  if (name == "passwords.dismissIssue" || name == "passwords.restoreIssue") {
    RejectCall(
        *call, BDK_CODE_INVALID_ARGUMENTS,
        "no password issue '" + *ArgumentsOf(*call).FindString("id") + "'");
    return true;
  }
  Profile* profile = ProfileOfCall(*call);
  Store store = profile ? ProfilePasswordStoreFactory::GetForProfile(
                              profile, ServiceAccessType::EXPLICIT_ACCESS)
                        : nullptr;
  if (!store) {
    RejectUnavailable(*call, "this profile has no password store");
    return true;
  }
  if (name == "passwords.list" || name == "passwords.forOrigin") {
    List(call, store);
  } else if (name == "passwords.reveal") {
    Reveal(call, store);
  } else if (name == "passwords.add") {
    Add(call, store);
  } else if (name == "passwords.update") {
    Update(call, store);
  } else if (name == "passwords.remove") {
    Remove(call, store);
  } else if (name == "passwords.neverSaved") {
    NeverSaved(call, store);
  } else if (name == "passwords.removeNeverSaved") {
    RemoveNeverSaved(call, store);
  } else if (name == "passwords.export") {
    Export(call, store);
  } else if (name == "passwords.import") {
    Import(call, store);
  } else {
    return false;
  }
  return true;
}

}  // namespace bdk
