#include "AzureIoTSasToken.h"
#include <az_iot.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform_util.h>
#include <time.h>

AzIoTSasToken::AzIoTSasToken(
    az_iot_hub_client* client, az_span deviceKey,
    az_span signatureBuffer, az_span sasTokenBuffer)
    : client(client), deviceKey(deviceKey), signatureBuffer(signatureBuffer),
      sasTokenBuffer(sasTokenBuffer), sasToken(AZ_SPAN_EMPTY), expirationUnixTime(0) {}

int AzIoTSasToken::Generate(unsigned int expiryTimeInMinutes) {
  sasToken = AZ_SPAN_EMPTY;
  const time_t now = time(NULL);
  if (now < 1600000000 || expiryTimeInMinutes == 0 || expiryTimeInMinutes > 1440)
    return -1;
  expirationUnixTime = uint32_t(now + expiryTimeInMinutes * 60UL);

  az_span signature;
  if (az_result_failed(az_iot_hub_client_sas_get_signature(
          client, expirationUnixTime, signatureBuffer, &signature))) return -1;

  uint8_t decodedKey[64];
  size_t decodedLength = 0;
  int rc = mbedtls_base64_decode(decodedKey, sizeof(decodedKey), &decodedLength,
                                az_span_ptr(deviceKey), az_span_size(deviceKey));
  if (rc != 0 || decodedLength == 0) {
    mbedtls_platform_zeroize(decodedKey, sizeof(decodedKey));
    Serial.println("Device key no es Base64 valido.");
    return -1;
  }

  uint8_t hmac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == NULL) {
    mbedtls_platform_zeroize(decodedKey, sizeof(decodedKey));
    return -1;
  }
  rc = mbedtls_md_hmac(info, decodedKey, decodedLength,
                       az_span_ptr(signature), az_span_size(signature), hmac);
  mbedtls_platform_zeroize(decodedKey, sizeof(decodedKey));
  if (rc != 0) return -1;

  unsigned char base64Hmac[64];
  size_t base64Length = 0;
  rc = mbedtls_base64_encode(base64Hmac, sizeof(base64Hmac), &base64Length,
                             hmac, sizeof(hmac));
  mbedtls_platform_zeroize(hmac, sizeof(hmac));
  if (rc != 0) return -1;

  // El SDK codifica URI y firma una sola vez y comprueba el espacio disponible.
  size_t passwordLength = 0;
  az_result result = az_iot_hub_client_sas_get_password(
      client, expirationUnixTime, az_span_create(base64Hmac, base64Length),
      AZ_SPAN_EMPTY, reinterpret_cast<char*>(az_span_ptr(sasTokenBuffer)),
      az_span_size(sasTokenBuffer), &passwordLength);
  if (az_result_failed(result)) return -1;
  sasToken = az_span_create(az_span_ptr(sasTokenBuffer), passwordLength);
  return 0;
}

bool AzIoTSasToken::IsExpired(unsigned int withinSeconds) {
  return time(NULL) + withinSeconds >= expirationUnixTime;
}

az_span AzIoTSasToken::Get() { return sasToken; }
