// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/client_cert_testing.h"

#include <memory>
#include <string>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "net/cert/x509_certificate.h"
#include "net/ssl/client_cert_identity.h"
#include "net/ssl/client_cert_store.h"
#include "net/ssl/openssl_private_key.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_private_key.h"
#include "third_party/boringssl/src/include/openssl/bio.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/pem.h"

namespace bdk {

namespace {

struct TestingIdentity {
  scoped_refptr<net::X509Certificate> certificate;
  scoped_refptr<net::SSLPrivateKey> key;
};

TestingIdentity& Current() {
  static base::NoDestructor<TestingIdentity> identity;
  return *identity;
}

class Identity : public net::ClientCertIdentity {
 public:
  Identity(scoped_refptr<net::X509Certificate> certificate,
           scoped_refptr<net::SSLPrivateKey> key)
      : net::ClientCertIdentity(std::move(certificate)), key_(std::move(key)) {}

  void AcquirePrivateKey(
      base::OnceCallback<void(scoped_refptr<net::SSLPrivateKey>)> done)
      override {
    std::move(done).Run(key_);
  }

 private:
  scoped_refptr<net::SSLPrivateKey> key_;
};

class Store : public net::ClientCertStore {
 public:
  explicit Store(TestingIdentity identity) : identity_(std::move(identity)) {}

  void GetClientCerts(scoped_refptr<const net::SSLCertRequestInfo> request,
                      ClientCertListCallback done) override {
    net::ClientCertIdentityList list;
    list.push_back(
        std::make_unique<Identity>(identity_.certificate, identity_.key));
    std::move(done).Run(std::move(list));
  }

 private:
  TestingIdentity identity_;
};

scoped_refptr<net::SSLPrivateKey> KeyFromPem(const std::string& pem) {
  bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(pem.data(), pem.size()));
  if (!bio) {
    return nullptr;
  }
  bssl::UniquePtr<EVP_PKEY> key(
      PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
  return key ? net::WrapOpenSSLPrivateKey(std::move(key)) : nullptr;
}

}  // namespace

bool RunClientCertTestingCommand(const std::string& name,
                                 const base::DictValue& args,
                                 base::DictValue& result) {
  if (name != "clientcert.identity") {
    return false;
  }
  const std::string* certificate = args.FindString("certificate");
  const std::string* key = args.FindString("key");
  if (!certificate || !key) {
    Current() = TestingIdentity();
    result.Set("set", false);
    return true;
  }
  net::CertificateList certificates =
      net::X509Certificate::CreateCertificateListFromBytes(
          base::as_byte_span(*certificate), net::X509Certificate::FORMAT_AUTO);
  scoped_refptr<net::SSLPrivateKey> private_key = KeyFromPem(*key);
  if (certificates.empty() || !private_key) {
    return false;
  }
  Current() = TestingIdentity{certificates.front(), std::move(private_key)};
  result.Set("set", true);
  return true;
}

std::unique_ptr<net::ClientCertStore> CreateTestingClientCertStore() {
  const TestingIdentity& identity = Current();
  if (!identity.certificate) {
    return nullptr;
  }
  return std::make_unique<Store>(identity);
}

}  // namespace bdk
