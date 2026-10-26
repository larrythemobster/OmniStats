#pragma once

#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>

namespace UpdaterCommon {
    enum class AuthenticodeStatus { Valid,
                                    UntrustedRoot,
                                    Revoked,
                                    RevocationUnknown,
                                    Invalid };

    inline AuthenticodeStatus ClassifyAuthenticodeStatus(LONG status, DWORD chainErrors, bool revocationChecked = true) {
        if (status == static_cast<LONG>(CERT_E_REVOKED) || status == static_cast<LONG>(CRYPT_E_REVOKED) ||
            (chainErrors & CERT_TRUST_IS_REVOKED)) {
            return AuthenticodeStatus::Revoked;
        }
        const bool acceptableStatus = status == ERROR_SUCCESS || status == static_cast<LONG>(CERT_E_UNTRUSTEDROOT);
        const bool unknownStatus = status == static_cast<LONG>(CERT_E_REVOCATION_FAILURE) ||
                                   status == static_cast<LONG>(CRYPT_E_REVOCATION_OFFLINE) ||
                                   status == static_cast<LONG>(CRYPT_E_NO_REVOCATION_CHECK);
        if (!acceptableStatus && !unknownStatus) return AuthenticodeStatus::Invalid;
        if (unknownStatus ||
            (revocationChecked && (chainErrors & (CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION)))) {
            return AuthenticodeStatus::RevocationUnknown;
        }
        if (status == ERROR_SUCCESS) return AuthenticodeStatus::Valid;
        if (status == static_cast<LONG>(CERT_E_UNTRUSTEDROOT)) return AuthenticodeStatus::UntrustedRoot;
        return AuthenticodeStatus::Invalid;
    }
}
