#include "AzureIoTSasToken.h"
#include <az_iot.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <Arduino.h>

// =======================================================
//   Helper: URL Encode (Para convertir +, /, = en %XX)
// =======================================================
void encodeURI(const char* src, char* dst) {
  char hex[] = "0123456789ABCDEF";
  while (*src) {
    if (isalnum(*src) || *src == '-' || *src == '_' || *src == '.' || *src == '~') {
      *dst++ = *src;
    } else {
      *dst++ = '%';
      *dst++ = hex[(*src >> 4) & 0x0F];
      *dst++ = hex[*src & 0x0F];
    }
    src++;
  }
  *dst = 0;
}

// =======================================================
//   Base64 Decode Helper
// =======================================================
static size_t base64Decode(const char* input, uint8_t* output) {
  size_t len = strlen(input);
  size_t outLen = 0;
  mbedtls_base64_decode(output, 64, &outLen, (const unsigned char*)input, len);
  return outLen;
}

// =======================================================
//   Base64 Encode Helper
// =======================================================
static size_t base64Encode(const uint8_t* input, size_t inputLen, char* output) {
  size_t outLen = 0;
  mbedtls_base64_encode((unsigned char*)output, 128, &outLen, input, inputLen);
  output[outLen] = 0; // Null terminate
  return outLen;
}

// =======================================================
//   Constructor
// =======================================================
AzIoTSasToken::AzIoTSasToken(
    az_iot_hub_client* client,
    az_span deviceKey,
    az_span signatureBuffer,
    az_span sasTokenBuffer)
{
  this->client = client;
  this->deviceKey = deviceKey;
  this->signatureBuffer = signatureBuffer;
  this->sasTokenBuffer = sasTokenBuffer;
  this->expirationUnixTime = 0;
  this->sasToken = AZ_SPAN_EMPTY;
}

// =======================================================
//   Generate SAS Token (CORREGIDO)
// =======================================================
int AzIoTSasToken::Generate(unsigned int expiryTimeInMinutes)
{
  expirationUnixTime = (uint32_t)(time(NULL) + expiryTimeInMinutes * 60);

  // 1. Obtener la firma cruda (resourceUri + \n + expiry)
  // Nota: Azure SDK ya maneja la codificación del resourceURI internamente aquí
  az_span signature = signatureBuffer;
  az_result rc = az_iot_hub_client_sas_get_signature(
      client,
      expirationUnixTime,
      signature,
      &signature);

  if (az_result_failed(rc)) {
    Serial.println("❌ Error construyendo string para firma");
    return -1;
  }

  // 2. Decodificar la Device Key (Base64 -> Bytes)
  uint8_t decoded_key[64];
  size_t decoded_key_len = base64Decode((const char*)az_span_ptr(deviceKey), decoded_key);

  // 3. HMAC-SHA256
  uint8_t hmac_result[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, decoded_key, decoded_key_len);
  mbedtls_md_hmac_update(&ctx, az_span_ptr(signatureBuffer), az_span_size(signature));
  mbedtls_md_hmac_finish(&ctx, hmac_result);
  mbedtls_md_free(&ctx);

  // 4. Base64 Encode del resultado HMAC
  char b64_hmac[128];
  base64Encode(hmac_result, 32, b64_hmac);

  // 5. URL ENCODE DE LA FIRMA (¡AQUÍ ESTABA EL ERROR!) 
  // La firma Base64 tiene +, /, = que rompen la URL si no se escapan.
  char encoded_signature[256];
  encodeURI(b64_hmac, encoded_signature);

  // 6. URL Encode del Resource URI también (Host/devices/id)
  char resourceUri[128];
  // Reconstruimos el resource URI que usa Azure SDK: "host/devices/id"
  snprintf(resourceUri, sizeof(resourceUri), "%s/devices/%s", 
           (char*)az_span_ptr(client->_internal.iot_hub_hostname),
           (char*)az_span_ptr(client->_internal.device_id));
  
  char encoded_resourceUri[256];
  encodeURI(resourceUri, encoded_resourceUri);

  // 7. Construir Token Final
  // SharedAccessSignature sr=<URL_ENCODED_URI>&sig=<URL_ENCODED_SIG>&se=<EXPIRY>
  int written = snprintf(
      (char*)az_span_ptr(sasTokenBuffer),
      az_span_size(sasTokenBuffer),
      "SharedAccessSignature sr=%s&sig=%s&se=%u",
      encoded_resourceUri,
      encoded_signature,
      (unsigned int)expirationUnixTime);

  if (written < 0) {
    Serial.println("❌ Error formateando Token final");
    return -1;
  }

  sasToken = az_span_create(az_span_ptr(sasTokenBuffer), written);
  return 0;
}

// =======================================================
//   IsExpired
// =======================================================
bool AzIoTSasToken::IsExpired() {
  return time(NULL) >= expirationUnixTime;
}

// =======================================================
//   Get
// =======================================================
az_span AzIoTSasToken::Get() {
  return sasToken;
}
