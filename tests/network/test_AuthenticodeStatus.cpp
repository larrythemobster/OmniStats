#include "updater/AuthenticodeStatus.hpp"
#include <gtest/gtest.h>

using UpdaterCommon::AuthenticodeStatus;
using UpdaterCommon::ClassifyAuthenticodeStatus;

TEST(AuthenticodeStatusTest, MapsTrustAndRevocationResults) {
    EXPECT_EQ(ClassifyAuthenticodeStatus(ERROR_SUCCESS, 0), AuthenticodeStatus::Valid);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CERT_E_UNTRUSTEDROOT), 0), AuthenticodeStatus::UntrustedRoot);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CERT_E_REVOKED), 0), AuthenticodeStatus::Revoked);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CRYPT_E_REVOKED), 0), AuthenticodeStatus::Revoked);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CERT_E_REVOCATION_FAILURE), 0), AuthenticodeStatus::RevocationUnknown);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CRYPT_E_REVOCATION_OFFLINE), 0), AuthenticodeStatus::RevocationUnknown);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CRYPT_E_NO_REVOCATION_CHECK), 0), AuthenticodeStatus::RevocationUnknown);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(TRUST_E_BAD_DIGEST), 0), AuthenticodeStatus::Invalid);
}

TEST(AuthenticodeStatusTest, ChainErrorsCannotBeMaskedByTrustStatus) {
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CERT_E_UNTRUSTEDROOT), CERT_TRUST_IS_REVOKED), AuthenticodeStatus::Revoked);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CERT_E_REVOCATION_FAILURE), CERT_TRUST_IS_REVOKED), AuthenticodeStatus::Revoked);
    EXPECT_EQ(ClassifyAuthenticodeStatus(ERROR_SUCCESS, CERT_TRUST_REVOCATION_STATUS_UNKNOWN), AuthenticodeStatus::RevocationUnknown);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CERT_E_UNTRUSTEDROOT), CERT_TRUST_IS_OFFLINE_REVOCATION), AuthenticodeStatus::RevocationUnknown);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(TRUST_E_BAD_DIGEST), CERT_TRUST_REVOCATION_STATUS_UNKNOWN), AuthenticodeStatus::Invalid);
}

TEST(AuthenticodeStatusTest, SignatureOnlyFallbackStillRejectsRevokedOrInvalidSignatures) {
    EXPECT_EQ(ClassifyAuthenticodeStatus(ERROR_SUCCESS, CERT_TRUST_REVOCATION_STATUS_UNKNOWN, false), AuthenticodeStatus::Valid);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CERT_E_UNTRUSTEDROOT), CERT_TRUST_IS_OFFLINE_REVOCATION, false), AuthenticodeStatus::UntrustedRoot);
    EXPECT_EQ(ClassifyAuthenticodeStatus(ERROR_SUCCESS, CERT_TRUST_IS_REVOKED, false), AuthenticodeStatus::Revoked);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CRYPT_E_REVOKED), 0, false), AuthenticodeStatus::Revoked);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(TRUST_E_BAD_DIGEST), CERT_TRUST_REVOCATION_STATUS_UNKNOWN, false), AuthenticodeStatus::Invalid);
    EXPECT_EQ(ClassifyAuthenticodeStatus(static_cast<LONG>(CRYPT_E_NO_REVOCATION_CHECK), 0, false), AuthenticodeStatus::RevocationUnknown);
}
