// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/autofill_service.h"

#include <array>
#include <functional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/services/service_common.h"
#include "chrome/browser/autofill/personal_data_manager_factory.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/global_features.h"
#include "chrome/browser/profiles/profile.h"
#include "components/application_locale_storage/application_locale_storage.h"
#include "components/autofill/core/browser/data_manager/addresses/address_data_manager.h"
#include "components/autofill/core/browser/data_manager/payments/payments_data_manager.h"
#include "components/autofill/core/browser/data_manager/personal_data_manager.h"
#include "components/autofill/core/browser/data_model/addresses/autofill_profile.h"
#include "components/autofill/core/browser/data_model/payments/credit_card.h"
#include "components/autofill/core/browser/field_types.h"

namespace bdk {

namespace {

using autofill::AutofillProfile;
using autofill::CreditCard;
using autofill::FieldType;
using bdkcore::BuiltinCall;

// The data manager writes asynchronously; a command answers once the change
// shows (or after this long, with what is there).
constexpr base::TimeDelta kSettleInterval = base::Milliseconds(10);
constexpr int kSettleAttempts = 300;

struct AddressField {
  const char* key;
  FieldType type;
};

constexpr std::array<AddressField, 9> kAddressFields = {{
    {"fullName", autofill::NAME_FULL},
    {"organization", autofill::COMPANY_NAME},
    {"street", autofill::ADDRESS_HOME_STREET_ADDRESS},
    {"city", autofill::ADDRESS_HOME_CITY},
    {"region", autofill::ADDRESS_HOME_STATE},
    {"postalCode", autofill::ADDRESS_HOME_ZIP},
    {"country", autofill::ADDRESS_HOME_COUNTRY},
    {"phone", autofill::PHONE_HOME_WHOLE_NUMBER},
    {"email", autofill::EMAIL_ADDRESS},
}};

std::string Locale() {
  return std::string(g_browser_process->GetFeatures()
                         ->application_locale_storage()
                         ->GetTag()
                         .tag_string());
}

base::DictValue FieldsOf(const AutofillProfile& profile) {
  base::DictValue fields;
  const std::string locale = Locale();
  for (const AddressField& field : kAddressFields) {
    const std::u16string value = profile.GetInfo(field.type, locale);
    if (!value.empty()) {
      fields.Set(field.key, base::UTF16ToUTF8(value));
    }
  }
  return fields;
}

base::DictValue InfoOf(const AutofillProfile& profile) {
  return base::DictValue()
      .Set("id", profile.guid())
      .Set("fields", FieldsOf(profile));
}

void ApplyFields(const base::DictValue& fields, AutofillProfile& profile) {
  const std::string locale = Locale();
  for (const AddressField& field : kAddressFields) {
    if (const std::string* value = fields.FindString(field.key)) {
      profile.SetInfo(field.type, base::UTF8ToUTF16(*value), locale);
    }
  }
}

base::DictValue InfoOf(const CreditCard& card) {
  return base::DictValue()
      .Set("id", card.guid())
      .Set("holder",
           base::UTF16ToUTF8(card.GetRawInfo(autofill::CREDIT_CARD_NAME_FULL)))
      .Set("network", card.network())
      .Set("lastFour", base::UTF16ToUTF8(card.LastFourDigits()))
      .Set("expiryMonth", card.expiration_month())
      .Set("expiryYear", card.expiration_year());
}

void ApplyCard(const base::DictValue& input, CreditCard& card) {
  if (const std::string* holder = input.FindString("holder")) {
    card.SetRawInfo(autofill::CREDIT_CARD_NAME_FULL,
                    base::UTF8ToUTF16(*holder));
  }
  if (const std::string* number = input.FindString("number")) {
    card.SetNumber(base::UTF8ToUTF16(*number));
  }
  if (std::optional<int> month = input.FindInt("expiryMonth")) {
    card.SetExpirationMonth(*month);
  }
  if (std::optional<int> year = input.FindInt("expiryYear")) {
    card.SetExpirationYear(*year);
  }
}

// Calls `done` once `settled` holds, polling the data manager's own state.
void WhenSettled(std::function<bool()> settled,
                 base::OnceClosure done,
                 int attempts_left = kSettleAttempts) {
  if (settled() || attempts_left <= 0) {
    std::move(done).Run();
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&WhenSettled, std::move(settled), std::move(done),
                     attempts_left - 1),
      kSettleInterval);
}

void RejectNo(const BuiltinCall& call,
              const char* what,
              const std::string& id) {
  RejectCall(call, BDK_CODE_INVALID_ARGUMENTS,
             std::string("no saved ") + what + " '" + id + "'");
}

void ExecuteAddresses(const std::shared_ptr<BuiltinCall>& call,
                      autofill::PersonalDataManager& pdm) {
  const std::string& name = call->name;
  const base::DictValue args = ArgumentsOf(*call);
  autofill::AddressDataManager* addresses = &pdm.address_data_manager();
  const base::WeakPtr<autofill::PersonalDataManager> weak = pdm.GetWeakPtr();
  auto manager = [weak]() -> autofill::AddressDataManager* {
    return weak ? &weak->address_data_manager() : nullptr;
  };

  if (name == "autofill.addresses.list") {
    base::ListValue items;
    for (const AutofillProfile* profile : addresses->GetProfilesForSettings()) {
      items.Append(InfoOf(*profile));
    }
    call->control->Resolve(
        ToJson(base::DictValue().Set("items", std::move(items))));
    return;
  }
  if (name == "autofill.addresses.add") {
    AutofillProfile profile(AutofillProfile::RecordType::kLocalOrSyncable,
                            autofill::AddressCountryCode("US"));
    if (const base::DictValue* fields = args.FindDict("fields")) {
      ApplyFields(*fields, profile);
    }
    const std::string guid = profile.guid();
    addresses->AddProfile(profile);
    WhenSettled(
        [manager, guid] {
          auto* m = manager();
          return !m || m->GetProfileByGUID(guid);
        },
        base::BindOnce(
            [](std::shared_ptr<BuiltinCall> call,
               std::function<autofill::AddressDataManager*()> manager,
               std::string guid) {
              auto* m = manager();
              const AutofillProfile* saved =
                  m ? m->GetProfileByGUID(guid) : nullptr;
              if (saved) {
                call->control->Resolve(ToJson(InfoOf(*saved)));
              } else {
                RejectCall(*call, "failed", "the address was not saved");
              }
            },
            call, manager, guid));
    return;
  }
  const std::string id = *args.FindString("id");
  const AutofillProfile* existing = addresses->GetProfileByGUID(id);
  if (!existing) {
    RejectNo(*call, "address", id);
    return;
  }
  if (name == "autofill.addresses.update") {
    AutofillProfile updated(*existing);
    if (const base::DictValue* fields = args.FindDict("fields")) {
      ApplyFields(*fields, updated);
    }
    const std::string wanted = ToJson(FieldsOf(updated));
    addresses->UpdateProfile(updated);
    WhenSettled(
        [manager, id, wanted] {
          auto* m = manager();
          const AutofillProfile* now = m ? m->GetProfileByGUID(id) : nullptr;
          return !now || ToJson(FieldsOf(*now)) == wanted;
        },
        base::BindOnce(
            [](std::shared_ptr<BuiltinCall> call,
               std::function<autofill::AddressDataManager*()> manager,
               std::string id) {
              auto* m = manager();
              const AutofillProfile* saved =
                  m ? m->GetProfileByGUID(id) : nullptr;
              if (saved) {
                call->control->Resolve(ToJson(InfoOf(*saved)));
              } else {
                RejectNo(*call, "address", id);
              }
            },
            call, manager, id));
    return;
  }
  // autofill.addresses.remove
  addresses->RemoveProfile(id);
  WhenSettled(
      [manager, id] {
        auto* m = manager();
        return !m || !m->GetProfileByGUID(id);
      },
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call) {
            call->control->Resolve("null");
          },
          call));
}

void ExecuteCards(const std::shared_ptr<BuiltinCall>& call,
                  autofill::PersonalDataManager& pdm) {
  const std::string& name = call->name;
  const base::DictValue args = ArgumentsOf(*call);
  autofill::PaymentsDataManager* cards = &pdm.payments_data_manager();
  const base::WeakPtr<autofill::PersonalDataManager> weak = pdm.GetWeakPtr();
  auto manager = [weak]() -> autofill::PaymentsDataManager* {
    return weak ? &weak->payments_data_manager() : nullptr;
  };

  if (name == "autofill.cards.list") {
    base::ListValue items;
    for (const CreditCard* card : cards->GetLocalCreditCards()) {
      items.Append(InfoOf(*card));
    }
    call->control->Resolve(
        ToJson(base::DictValue().Set("items", std::move(items))));
    return;
  }
  if (name == "autofill.cards.add") {
    CreditCard card(base::Uuid::GenerateRandomV4().AsLowercaseString());
    if (const base::DictValue* input = args.FindDict("card")) {
      ApplyCard(*input, card);
    }
    const std::string guid = card.guid();
    cards->AddCreditCard(card);
    WhenSettled(
        [manager, guid] {
          auto* m = manager();
          return !m || m->GetCreditCardByGUID(guid);
        },
        base::BindOnce(
            [](std::shared_ptr<BuiltinCall> call,
               std::function<autofill::PaymentsDataManager*()> manager,
               std::string guid) {
              auto* m = manager();
              const CreditCard* saved =
                  m ? m->GetCreditCardByGUID(guid) : nullptr;
              if (saved) {
                call->control->Resolve(ToJson(InfoOf(*saved)));
              } else {
                RejectCall(*call, "failed", "the card was not saved");
              }
            },
            call, manager, guid));
    return;
  }
  const std::string id = *args.FindString("id");
  const CreditCard* existing = cards->GetCreditCardByGUID(id);
  if (!existing ||
      existing->record_type() != CreditCard::RecordType::kLocalCard) {
    RejectNo(*call, "card", id);
    return;
  }
  if (name == "autofill.cards.reveal") {
    call->control->Resolve(ToJson(base::DictValue().Set(
        "number", base::UTF16ToUTF8(existing->number()))));
    return;
  }
  if (name == "autofill.cards.update") {
    CreditCard updated(*existing);
    if (const base::DictValue* input = args.FindDict("card")) {
      ApplyCard(*input, updated);
    }
    const std::string wanted = ToJson(InfoOf(updated));
    cards->UpdateCreditCard(updated);
    WhenSettled(
        [manager, id, wanted] {
          auto* m = manager();
          const CreditCard* now = m ? m->GetCreditCardByGUID(id) : nullptr;
          return !now || ToJson(InfoOf(*now)) == wanted;
        },
        base::BindOnce(
            [](std::shared_ptr<BuiltinCall> call,
               std::function<autofill::PaymentsDataManager*()> manager,
               std::string id) {
              auto* m = manager();
              const CreditCard* saved =
                  m ? m->GetCreditCardByGUID(id) : nullptr;
              if (saved) {
                call->control->Resolve(ToJson(InfoOf(*saved)));
              } else {
                RejectNo(*call, "card", id);
              }
            },
            call, manager, id));
    return;
  }
  // autofill.cards.remove
  cards->RemoveByGUID(id);
  WhenSettled(
      [manager, id] {
        auto* m = manager();
        return !m || !m->GetCreditCardByGUID(id);
      },
      base::BindOnce(
          [](std::shared_ptr<BuiltinCall> call) {
            call->control->Resolve("null");
          },
          call));
}

}  // namespace

bool ExecuteAutofillCommand(bdkcore::Runtime& rt,
                            const std::shared_ptr<BuiltinCall>& call) {
  if (!call->name.starts_with("autofill.")) {
    return false;
  }
  Profile* profile = ProfileOfCall(*call);
  autofill::PersonalDataManager* pdm =
      profile
          ? autofill::PersonalDataManagerFactory::GetForBrowserContext(profile)
          : nullptr;
  if (!pdm) {
    RejectUnavailable(*call, "this profile has no autofill data");
    return true;
  }
  if (call->name.starts_with("autofill.addresses.")) {
    ExecuteAddresses(call, *pdm);
  } else if (call->name.starts_with("autofill.cards.")) {
    ExecuteCards(call, *pdm);
  } else {
    return false;
  }
  return true;
}

}  // namespace bdk
