/*
 * There are three or four incompatible memory allocators:
 *
 *   palloc / pfree (Postgres-specific)
 *   malloc / free
 *   new / delete
 *   new[] / delete[]
 *
 * The Postgres versions of the UDFs likely do not keep track of which
 * allocator is used in each case.  They might not free memory at all
 * in some cases, and might free memory with a different allocator than
 * the one used to initially allocate it.  Beware.
 *
 * The MySQL versions of the UDFs are more likely to get this right.
 */

/*
 * Handling NULL value
 * - Encryption routines should never get a NULL value due to the way
 *   rewriting is handled for NULL.
 * - Decryption routines should forgo the decryption of NULL values.
 */

#define DEBUG 1

#include <memory>

#include <crypto/BasicCrypto.hh>
#include <crypto/blowfish.hh>
#include <crypto/SWPSearch.hh>
#include <crypto/paillier.hh>
#include <util/params.hh>
#include <util/util.hh>

using namespace NTL;

extern "C" {


typedef unsigned long long ulonglong;
typedef long long longlong;
#include <mysql.h>
#include <ctype.h>

my_bool decrypt_int_sem_init(UDF_INIT *const initid, UDF_ARGS *const args,
                             char *const message);
ulonglong decrypt_int_sem(UDF_INIT *const initid, UDF_ARGS *const args,
                          char *const is_null, char *const error);

my_bool decrypt_int_det_init(UDF_INIT *const initid, UDF_ARGS *const args,
                              char *const message);
ulonglong decrypt_int_det(UDF_INIT *const initid, UDF_ARGS *const args,
                          char *const is_null, char *const error);

my_bool decrypt_text_sem_init(UDF_INIT *const initid,
                              UDF_ARGS *const args, char *const message);
void decrypt_text_sem_deinit(UDF_INIT *const initid);
char *decrypt_text_sem(UDF_INIT *const initid, UDF_ARGS *const args,
                       char *const result, unsigned long *const length,
                       char *const is_null, char *const error);

my_bool encrypt_int_det_init(UDF_INIT *const initid, UDF_ARGS *const args,
                             char *const message);
ulonglong encrypt_int_det(UDF_INIT *const initid, UDF_ARGS *const args,
                          char *const is_null, char *const error);

my_bool decrypt_text_det_init(UDF_INIT *const initid, UDF_ARGS *const args,
                              char *const message);
void decrypt_text_det_deinit(UDF_INIT *const initid);
char *decrypt_text_det(UDF_INIT *const initid, UDF_ARGS *const args,
                       char *const result, unsigned long *const length,
                       char *const is_null, char *const error);

my_bool search_init(UDF_INIT *const initid, UDF_ARGS *const args,
                    char *const message);
ulonglong search(UDF_INIT *const initid, UDF_ARGS *const args,
                 char *const is_null, char *const error);

my_bool searchSWP_init(UDF_INIT *const initid, UDF_ARGS *const args,
                       char *const message);
void searchSWP_deinit(UDF_INIT *const initid);
ulonglong searchSWP(UDF_INIT *const initid, UDF_ARGS *const args,
                    char *const is_null, char *const error);

my_bool agg_init(UDF_INIT *const initid, UDF_ARGS *const args,
                 char *const message);
void agg_deinit(UDF_INIT *const initid);
void agg_clear(UDF_INIT *const initid, char *const is_null,
               char *const error);
my_bool agg_add(UDF_INIT *const initid, UDF_ARGS *const args,
                char *const is_null, char *const error);
char *agg(UDF_INIT *const initid, UDF_ARGS *const args,
          char *const result, unsigned long *const length,
          char *const is_null, char *const error);

void func_add_set_deinit(UDF_INIT *const initid);
char *func_add_set(UDF_INIT *const initid, UDF_ARGS *const args,
                   char *const result, unsigned long *const length,
                   char *const is_null, char *const error);
}

static void __attribute__((unused))
log(const std::string &s)
{
    /* Writes to the server's error log */
    fprintf(stderr, "%s\n", s.c_str());
}


static std::string
decrypt_SEM(const unsigned char *const eValueBytes, uint64_t eValueLen,
            AES_KEY *const aesKey, uint64_t salt)
{
    std::string c(reinterpret_cast<const char *>(eValueBytes),
                  static_cast<unsigned int>(eValueLen));
    return decrypt_AES_CBC(c, aesKey, BytesFromInt(salt, SALT_LEN_BYTES),
                           false);
}


static std::unique_ptr<std::list<std::string>>
split(const std::string &s, unsigned int plen)
{
    const unsigned int len = s.length();
    if (len % plen != 0) {
        thrower() << "split receives invalid input";
    }

    const unsigned int num = len / plen;
    std::unique_ptr<std::list<std::string>>
        res(new std::list<std::string>());

    for (unsigned int i = 0; i < num; i++) {
        res.get()->push_back(s.substr(i*plen, plen));
    }

    return res;
}


static bool
searchExists(const Token &token, const std::string &overall_ciph)
{
    const std::unique_ptr<std::list<std::string>>
        l(split(overall_ciph, SWPCiphSize));
    return SWP::searchExists(token, *l.get());
}


static bool
search(const Token &token, const std::string &overall_ciph)
{
   return searchExists(token, overall_ciph);
}


#if MYSQL_S
#define ARGS args

static uint64_t
getui(UDF_ARGS *const args, int i)
{
    return static_cast<uint64_t>(*reinterpret_cast<ulonglong *>(args->args[i]));
}

static char *
getba(UDF_ARGS *const args, int i, uint64_t &len)
{
    len = args->lengths[i];
    return args->args[i];
}

#else

#define ARGS PG_FUNCTION_ARGS

static uint64_t
getui(ARGS, int i)
{
    return PG_GETARG_INT64(i);
}

static unsigned char
getb(ARGS, int i)
{
    return static_cast<unsigned char>(PG_GETARG_INT32(i));
}

static unsigned char *
getba(ARGS, int i, unsigned int &len)
{
    bytea *const eValue = PG_GETARG_BYTEA_P(i);

    len = VARSIZE(eValue) - VARHDRSZ;
    unsigned char *const  eValueBytes = new unsigned char[len];
    memcpy(eValueBytes, VARDATA(eValue), len);
    return eValueBytes;
}

#endif

extern "C" {

#if MYSQL_S
my_bool
decrypt_int_sem_init(UDF_INIT *const initid, UDF_ARGS *const args,
                     char *const message)
{
    initid->maybe_null = 1;
    return 0;
}

ulonglong
decrypt_int_sem(UDF_INIT *const initid, UDF_ARGS *const args,
                char *const is_null, char *const error)
#else /*postgres*/
Datum
decrypt_int_sem(PG_FUNCTION_ARGS)
#endif
{
    AssignOnce<uint64_t> value;
    if (NULL == ARGS->args[0]) {
        value = 0;
        *is_null = 1;
    } else {
        const uint64_t eValue = getui(ARGS, 0);

        uint64_t keyLen;
        char *const keyBytes = getba(args, 1, keyLen);
        const std::string key = std::string(keyBytes, keyLen);

        uint64_t salt = getui(args, 2);

        blowfish bf(key);
        value = bf.decrypt(eValue) ^ salt;
    }

    //cerr << "udf: encVal " << eValue << " key " << (int)key[0] << " " << (int)key[1] << " " << (int) key[3]  << " salt " << salt  << " obtains: " << value << " and cast to ulonglong " << (ulonglong) value << "\n";


#if MYSQL_S
     return static_cast<ulonglong>(value.get());
#else /* postgres */
    PG_RETURN_INT64(value.get());
#endif
}



#if MYSQL_S
my_bool
decrypt_int_det_init(UDF_INIT *const initid, UDF_ARGS *const args,
                     char *const message)
{
    initid->maybe_null = 1;
    return 0;
}

ulonglong
decrypt_int_det(UDF_INIT *const initid, UDF_ARGS *const args,
                char *const is_null, char *const error)
#else /* postgres */
Datum
decrypt_int_det(PG_FUNCTION_ARGS)
#endif
{
    AssignOnce<uint64_t> value;
    if (NULL == ARGS->args[0]) {
        value = 0;
        *is_null = 1;
    } else {
        const uint64_t eValue = getui(ARGS, 0);

        uint64_t keyLen;
        char *const keyBytes = getba(args, 1, keyLen);
        const std::string key = std::string(keyBytes, keyLen);

        const uint64_t shift = getui(ARGS, 2);

        blowfish bf(key);
        value = bf.decrypt(eValue) - shift;
    }


#if MYSQL_S
    return static_cast<ulonglong>(value.get());
#else /* postgres */
    PG_RETURN_INT64(value.get());
#endif

}



my_bool
decrypt_text_sem_init(UDF_INIT *const initid, UDF_ARGS *const args,
                      char *const message)
{
    initid->maybe_null = 1;
    return 0;
}

void
decrypt_text_sem_deinit(UDF_INIT *const initid)
{
    /*
     * in mysql-server/sql/item_func.cc, udf_handler::fix_fields
     * initializes initid.ptr=0 for us.
     */
    if (initid->ptr)
        delete[] initid->ptr;
}

char *
decrypt_text_sem(UDF_INIT *const initid, UDF_ARGS *const args,
                 char *const result, unsigned long *const length,
                 char *const is_null, char *const error)
{
    AssignOnce<std::string> value;
    if (NULL == ARGS->args[0]) {
        value = "";
        *is_null = 1;
    } else {
        uint64_t eValueLen;
        char *const eValueBytes = getba(args, 0, eValueLen);

        uint64_t keyLen;
        char *const keyBytes = getba(args, 1, keyLen);
        const std::string key = std::string(keyBytes, keyLen);

        uint64_t salt = getui(ARGS, 2);

        const std::unique_ptr<AES_KEY> aesKey(get_AES_dec_key(key));
        value =
            decrypt_SEM(reinterpret_cast<unsigned char *>(eValueBytes),
                        eValueLen, aesKey.get(), salt);
    }

    // NOTE: This is not creating a proper C string, no guarentee of NUL
    // termination.
    char *const res = new char[value.get().length()];
    initid->ptr = res;
    memcpy(res, value.get().data(), value.get().length());
    *length = value.get().length();

    return static_cast<char*>(initid->ptr);
}



#if MYSQL_S
my_bool
encrypt_int_det_init(UDF_INIT *const initid, UDF_ARGS *const args,
                     char *const message)
{
    return 0;
}

ulonglong
encrypt_int_det(UDF_INIT *const initid, UDF_ARGS *const args,
                char *const is_null, char *const error)
#else /* postgres */
Datum
decrypt_int_det(PG_FUNCTION_ARGS)
#endif
{
    const uint64_t eValue = getui(ARGS, 0);

    uint64_t keyLen;
    char *const keyBytes = getba(args, 1, keyLen);
    const std::string key = std::string(keyBytes, keyLen);

    blowfish bf(key);
    const uint64_t value = bf.encrypt(eValue);

#if MYSQL_S
    return static_cast<ulonglong>(value);
#else /* postgres */
    PG_RETURN_INT64(value);
#endif

}


#if MYSQL_S
my_bool
decrypt_text_det_init(UDF_INIT *const initid, UDF_ARGS *const args,
                      char *const message)
{
    return 0;
}

void
decrypt_text_det_deinit(UDF_INIT *const initid)
{
    /*
     * in mysql-server/sql/item_func.cc, udf_handler::fix_fields
     * initializes initid.ptr=0 for us.
     */
    if (initid->ptr)
        delete[] initid->ptr;
}

char *
decrypt_text_det(UDF_INIT *const initid, UDF_ARGS *const args,
                 char *const result, unsigned long *const length,
                 char *const is_null, char *const error)
#else /* postgres */
Datum
decrypt_text_det(PG_FUNCTION_ARGS)
#endif
{
    uint64_t eValueLen;
    char *const eValueBytes = getba(args, 0, eValueLen);

    uint64_t keyLen;
    char *const keyBytes = getba(args, 1, keyLen);
    const std::string key = std::string(keyBytes, keyLen);

    const std::unique_ptr<AES_KEY> aesKey(get_AES_dec_key(key));
    const std::string value =
        decrypt_AES_CMC(std::string(eValueBytes,
                                    static_cast<unsigned int>(eValueLen)),
                        aesKey.get(), true);

#if MYSQL_S
    char *const res = new char[value.length()];
    initid->ptr = res;
    memcpy(res, value.data(), value.length());
    *length = value.length();
    return initid->ptr;
#else
    bytea *const res = (bytea *) palloc(eValueLen+VARHDRSZ);
    SET_VARSIZE(res, eValueLen+VARHDRSZ);
    memcpy(VARDATA(res), value, eValueLen);
    PG_RETURN_BYTEA_P(res);
#endif

}

/*
 * given field of the form:   len1 word1 len2 word2 len3 word3 ...,
 * where each len is the length of the following "word",
 * search for word which is of the form len word_body where len is
 * the length of the word body
 */


#if MYSQL_S
my_bool
search_init(UDF_INIT *const initid, UDF_ARGS *const args,
            char *const message)
{
    return 0;
}


ulonglong
search(UDF_INIT *const initid, UDF_ARGS *const args, char *const is_null,
       char *const error)
#else
Datum
search(PG_FUNCTION_ARGS)
#endif
{
    uint64_t wordLen;
    char *word = getba(ARGS, 0, wordLen);
    if (wordLen != static_cast<unsigned int>(word[0])) {
        std::cerr << "ERR: wordLen is not equal to fist byte of word!!! ";
    }
    word = word + 1;     // +1 skips over the length field
    //cerr << "given expr to search for has " << wordLen << " length \n";

    uint64_t fieldLen;
    char *const field = getba(ARGS, 1, fieldLen);

    //cerr << "searching for "; myPrint((unsigned char *)word, wordLen); cerr
    // << " in field "; myPrint((unsigned char *)field, fieldLen); cerr <<
    // "\n";

    unsigned int i = 0;
    while (i < fieldLen) {
        const unsigned int currLen = static_cast<unsigned int>(field[i]);
        if (currLen != wordLen) {
            i = i + currLen+1;
            continue;
        }

        //need to compare
        unsigned int j;
        for (j = 0; j < currLen; j++) {
            if (field[i+j+1] != word[j]) {
                break;
            }
        }
        if (j == currLen) {
#if MYSQL_S
            return 1;
#else
            PG_RETURN_BOOL(true);
#endif
        }
        i = i + currLen + 1;
    }

#if MYSQL_S
    return 0;
#else
    PG_RETURN_BOOL(true);
#endif
}

#if MYSQL_S

//TODO: write a version of search for postgres

my_bool
searchSWP_init(UDF_INIT *const initid, UDF_ARGS *const args,
               char *const message)
{
    Token *const t = new Token();

    uint64_t ciphLen;
    char *const ciph = getba(args, 1, ciphLen);

    uint64_t wordKeyLen;
    char *const wordKey = getba(args, 2, wordKeyLen);

    t->ciph = std::string(ciph, ciphLen);
    t->wordKey = std::string(wordKey, wordKeyLen);

    initid->ptr = reinterpret_cast<char *>(t);

    return 0;
}

void
searchSWP_deinit(UDF_INIT *const initid)
{
    Token *const t = reinterpret_cast<Token *>(initid->ptr);
    delete t;
}

ulonglong
searchSWP(UDF_INIT *const initid, UDF_ARGS *const args,
          char *const is_null, char *const error)
{
    uint64_t allciphLen;
    char *const allciph = getba(ARGS, 0, allciphLen);
    const std::string overallciph = std::string(allciph, allciphLen);

    Token *const t = reinterpret_cast<Token *>(initid->ptr);

    return search(*t, overallciph);
}

#endif




struct agg_state {
    ZZ sum;
    ZZ n2;
    bool n2_set;
    void *rbuf;
};

my_bool
agg_init(UDF_INIT *const initid, UDF_ARGS *const args, char *const message)
{
    std::cerr << "in agg_init \n";
    agg_state *const as = new agg_state();
    as->rbuf = malloc(Paillier_len_bytes);
    initid->ptr = reinterpret_cast<char *>(as);
    initid->maybe_null = 1;
    std::cerr << "returning from agg_init \n";
    return 0;
}

void
agg_deinit(UDF_INIT *const initid)
{
    agg_state *const as = reinterpret_cast<agg_state *>(initid->ptr);
    free(as->rbuf);
    delete as;
}

// When we want to add by zero for HOM values we can multiply our value
// by 1.
void
agg_clear(UDF_INIT *const initid, char *const is_null, char *const error)
{
    agg_state *const as = reinterpret_cast<agg_state *>(initid->ptr);
    as->sum = to_ZZ(1);
    as->n2_set = 0;
}

//args will be element to add, constant N2
my_bool
agg_add(UDF_INIT *const initid, UDF_ARGS *const args, char *const is_null,
        char *const error)
{
    //cerr << "in agg_add \n";
    agg_state *const as = reinterpret_cast<agg_state *>(initid->ptr);
    if (!as->n2_set) {
        //cerr << "n2 length is " << args->lengths[1] << "\n";
        //cerr << "n2 first byte is " << (int)args->args[1][0] << "\n";
        ZZFromBytes(as->n2,
                    reinterpret_cast<const uint8_t *>(args->args[1]),
                    args->lengths[1]);
        //cerr << "n2 is " << as->n2 << "\n";
        as->n2_set = 1;
    }

    ZZ e;
    if (NULL == args->args[0]) {
        e = to_ZZ(1);
    } else {
        ZZFromBytes(e, reinterpret_cast<const uint8_t *>(args->args[0]),
                    args->lengths[0]);
    }

    //cerr << "element to add " << e << "\n";
    MulMod(as->sum, as->sum, e, as->n2);
    //cerr << "sum so far " << as->sum << "\n";
    return true;
}

char *
agg(UDF_INIT *const initid, UDF_ARGS *const args, char *const result,
    unsigned long *const length, char *const is_null, char *const error)
{
    agg_state *const as = reinterpret_cast<agg_state *>(initid->ptr);
    BytesFromZZ(static_cast<uint8_t *>(as->rbuf), as->sum,
                Paillier_len_bytes);
    *length = Paillier_len_bytes;
    return static_cast<char *>(as->rbuf);
}

// for update with increment

void
func_add_set_deinit(UDF_INIT *const initid)
{
    if (initid->ptr)
        free(initid->ptr);
}

char *
func_add_set(UDF_INIT *const initid, UDF_ARGS *const args,
             char *const result, unsigned long *const length,
             char *const is_null, char *const error)
{
    if (initid->ptr)
        free(initid->ptr);

    AssignOnce<uint64_t> out_len;
    ZZ res;
    if (NULL == ARGS->args[0]) {
        out_len = 0;
        *is_null = 1;
        res = 0;
    } else {
        out_len = args->lengths[2];

        ZZ field, val, n2;
        ZZFromBytes(field,
                    reinterpret_cast<const uint8_t *>(args->args[0]),
                    args->lengths[0]);
        ZZFromBytes(val, reinterpret_cast<const uint8_t *>(args->args[1]),
                    args->lengths[1]);
        ZZFromBytes(n2, reinterpret_cast<const uint8_t *>(args->args[2]),
                    args->lengths[2]);

        MulMod(res, field, val, n2);
    }

    void *const rbuf = malloc(static_cast<size_t>(out_len.get()));
    initid->ptr = static_cast<char *>(rbuf);
    BytesFromZZ(static_cast<uint8_t *>(rbuf), res,
                static_cast<size_t>(out_len.get()));

    *length = static_cast<long unsigned int>(out_len.get());
    return initid->ptr;
}



} /* extern "C" */
