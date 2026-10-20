/* The adapter supplies CA PEM and the original TLS peer name from shard-owned
   startup configuration. Network connect receives a numeric IP after Seastar DNS;
   certificate verification must still use the configured name, without DNS I/O. */
#include <openssl/pem.h>
static const char* tls_peer_name(MYSQL* mysql)
{
    const char* name = NULL;
    mysql_get_optionv(mysql, MARIADB_OPT_USERDATA, "imm_tls_peer_name", &name);
    return name;
}
static int load_memory_ca(SSL_CTX* context, const char* pem, const char* directory)
{
    BIO* input;
    STACK_OF(X509_INFO) * certificates;
    X509_INFO* entry;
    int index, count = 0, result = 1;
    if (!pem || directory) {
        return 0;
    }
    input = BIO_new_mem_buf(pem, -1);
    if (!input) {
        return 0;
    }
    certificates = PEM_X509_INFO_read_bio(input, NULL, NULL, NULL);
    BIO_free(input);
    if (!certificates) {
        return 0;
    }
    for (index = 0; index < sk_X509_INFO_num(certificates); ++index) {
        entry = sk_X509_INFO_value(certificates, index);
        if (entry->x509) {
            ++count;
            if (X509_STORE_add_cert(SSL_CTX_get_cert_store(context), entry->x509) != 1) {
                result = 0;
                break;
            }
        }
    }
    sk_X509_INFO_pop_free(certificates, X509_INFO_free);
    return count && result;
}
