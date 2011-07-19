/**
 *  This is the crypto manager.
 */

#include <stdlib.h>

#include "CryptoManager.h"
#include "log.h"

/*
   //either provide a level key or provide a master key and the name of the
      field
   void EDBClient::encrypt(SECLEVEL seclevel, string plaintext, uint64_t salt,
      unsigned char * levelkey, unsigned char * mkey, string fieldname, string
      & ciphertext) {
        switch seclevel {
        case SEM: {}
        case DET: {}
        case OPE: {}
        case DETJOIN: {}
        case
        }
   }
   void EDBClient::decrypt(SECLEVEL seclevel, string & plaintext, uint64_t
      salt, unsigned char * levelkey, unsigned char * mkey, string fieldname,
      string ciphertext) {

   }
 */
// TODO: simplify CryptoManager using a function taking from level to level
// for a type of data using union for answers or inputs
// TODO: optimizations for CryptAPP, HOM especially; for each user, for each
// field, keep the keys in the right format already and just load them rather
// than recompute

static ZZ
Paillier_L(const ZZ & u, const ZZ & n)
{
    return (u - 1) / n;
}

static ZZ
LCM(const ZZ & a, const ZZ & b)
{
    return (a * b) / GCD(a, b);
}

AES_KEY *
CryptoManager::getmkey()
{
    return masterKey;
}

CryptoManager::CryptoManager(const string &masterKeyArg)
{
    VERBOSE = VERBOSE_G;
    masterKey = new AES_KEY();

    AES_set_encrypt_key((const uint8_t*) masterKeyArg.c_str(),
                        AES_KEY_SIZE, masterKey);

    RAND_seed((const uint8_t*) masterKeyArg.c_str(), MASTER_KEY_SIZE);

    SetSeed(ZZFromBytes((const uint8_t *) masterKeyArg.c_str(),
                        MASTER_KEY_SIZE));

    useEncTables = false;

    //setup Paillier encryption
    ZZ p, q;

    p = RandomPrime_ZZ(Paillier_len_bits/4);
    q = RandomPrime_ZZ(Paillier_len_bits/4);

    Paillier_n = p * q;
    Paillier_n2 = Paillier_n * Paillier_n;

    Paillier_lambda = LCM(p-1, q-1);

    //generate g

    do {

        Paillier_g = RandomLen_ZZ(Paillier_len_bits) % Paillier_n2;

    } while (GCD(Paillier_L(PowerMod(Paillier_g, Paillier_lambda,
                                     Paillier_n2),
                            Paillier_n), Paillier_n) != to_ZZ(1));

    Paillier_dec_denom =
        InvMod(Paillier_L(PowerMod(Paillier_g,  Paillier_lambda, Paillier_n2),
                          Paillier_n),
               Paillier_n);

}

SECLEVEL
highestEq(SECLEVEL sl)
{
    if (sl == SEMANTIC_DET) {
        return DET;
    } else {
        return sl;
    }
}

static onion
getOnion(SECLEVEL l1)
{
    switch (l1) {
    case PLAIN_DET: {return oDET; }
    case DETJOIN: {return oDET; }
    case DET: {return oDET; }
    case SEMANTIC_DET: {return oDET; }
    case PLAIN_OPE: {return oOPE; }
    case OPEJOIN: {return oOPE; }
    case OPESELF: {return oOPE; }
    case SEMANTIC_OPE: {return oOPE; }
    case PLAIN_AGG: {return oAGG; }
    case SEMANTIC_AGG: {return oAGG; }
    case PLAIN: {return oNONE; }
    default: {return oINVALID; }
    }
    return oINVALID;
}

static SECLEVEL
decreaseLevel(SECLEVEL l, fieldType ft,  onion o)
{
    switch (o) {
    case oDET: {
        switch (l) {
        case SEMANTIC_DET: {return DET; }
        case DET: {
            return DETJOIN;
        }
        case DETJOIN: {return PLAIN_DET; }
        default: {
            assert_s(false, "cannot decrease level");
            return INVALID;
        }
        }
    }
    case oOPE: {
        switch (l) {
        case SEMANTIC_OPE: {return OPESELF; }
        case OPESELF: {
            if (ft == TYPE_TEXT) {
                return PLAIN_OPE;
            } else {
                return OPEJOIN;
            }
        }
        case OPEJOIN: {return PLAIN_OPE; }
        default: {
            assert_s(false, "cannot decrease level");
            return INVALID;
        }
        }
    }
    case oAGG: {
        switch (l) {
        case SEMANTIC_AGG: {return PLAIN_AGG; }
        default: {
            assert_s(false, "cannot decrease level");
            return INVALID;
        }
        }
    }
    default: {
        assert_s(false, "cannot decrease level");
        return INVALID;
    }
    }

}

static SECLEVEL
increaseLevel(SECLEVEL l, fieldType ft, onion o)
{
    switch (o) {
    case oDET: {
        switch (l) {
        case DET: {return SEMANTIC_DET; }
        case DETJOIN: {return DET; }
        case PLAIN_DET: {
            return DETJOIN;
        }
        default: {
            assert_s(false, "cannot increase level");
            return INVALID;
        }
        }
    }
    case oOPE: {
        switch (l) {
        case OPESELF: {return SEMANTIC_OPE; }
        case OPEJOIN: {return OPESELF; }
        case PLAIN_OPE: {
            if (ft == TYPE_TEXT) {
                return OPESELF;
            } else {
                return OPEJOIN;
            }
        }

        default: {
            assert_s(false, "cannot increase level");
            return INVALID;
        }
        }
    }
    case oAGG: {
        switch (l) {
        case PLAIN_AGG: {return SEMANTIC_AGG; }
        default: {
            assert_s(false, "cannot increase level");
            return INVALID;
        }
        }
    }
    default: {
        assert_s(false, "cannot increase level");
        return INVALID;
    }
    }

}

//////////////////////////////////////////////////////////////////

//TODO: optimization: crypt can take in an array of elements to decrypt as
// opposed to just one field
// when we want to decrypt many items from a column, in this way, we do not
// need to construct the key every time
string
CryptoManager::crypt(AES_KEY * mkey, string data, fieldType ft,
                     string fullfieldname,
                     SECLEVEL fromlevel, SECLEVEL tolevel,
                     uint64_t salt)
{

    //cerr << "+ crypt: salt " << salt << " data " << data << " fullfieldname
    // " << fullfieldname << " fromlevel " << fromlevel << " to level" <<
    // tolevel << "\n";
    onion o = getOnion(fromlevel);

    myassert((o != oINVALID) && (o == getOnion(
                                     tolevel)),
             "levels for crypt are not on the same onion");

    int comp = fromlevel - tolevel;

    if (comp == 0) {
        //do nothing
        return data;
    }

    if (comp > 0) {
        //need to decrypt

        switch (ft) {
        case TYPE_INTEGER: {

            switch (o) {
            case oDET: {
                uint64_t val = unmarshallVal(data);
                if (fromlevel == SEMANTIC_DET) {
                    AES_KEY * key =
                        get_key_SEM(getKey(mkey, fullfieldname, fromlevel));
                    val = decrypt_SEM(val, key, salt);
                    fromlevel  = decreaseLevel(fromlevel, ft, oDET);
                    if (fromlevel == tolevel) {
                        return marshallVal(val);
                    }
                }

                if (fromlevel == DET) {
                    AES_KEY * key =
                        get_key_DET(getKey(mkey, fullfieldname, fromlevel));
                    val = decrypt_DET(val, key);
                    fromlevel = decreaseLevel(fromlevel, ft, oDET);
                    if (fromlevel == tolevel) {
                        return marshallVal(val);
                    }
                }

                if (fromlevel == DETJOIN) {
                    AES_KEY * key =
                        get_key_DET(getKey(mkey, "join", fromlevel));
                    val = decrypt_DET(val, key);
                    fromlevel = decreaseLevel(fromlevel, ft, oDET);
                    if (fromlevel == tolevel) {
                        return marshallVal(val);
                    }
                }

                assert_s(false, "nothing lower than plain");

                return "";
            }
            case oOPE: {
                uint64_t val = unmarshallVal(data);
                if (fromlevel == SEMANTIC_OPE) {
                    AES_KEY * key =
                        get_key_SEM(getKey(mkey, fullfieldname, fromlevel));
                    val = decrypt_SEM(val, key, salt);
                    fromlevel  = decreaseLevel(fromlevel, ft, oOPE);
                    if (fromlevel == tolevel) {
                        return marshallVal(val);
                    }
                }

                if (fromlevel == OPESELF) {
                    OPE * key =
                        get_key_OPE(getKey(mkey, fullfieldname, fromlevel));
                    val = decrypt_OPE(val, key);
                    fromlevel = decreaseLevel(fromlevel, ft, oOPE);
                    if (fromlevel == tolevel) {
                        return marshallVal(val);
                    }
                }

                if (fromlevel == OPEJOIN) {
                    fromlevel = decreaseLevel(fromlevel, ft, oOPE);
                    if (fromlevel == tolevel) {
                        return marshallVal(val);
                    }
                }

                assert_s(false, "nothing lower than plain ope");

                return "";
            }
            case oAGG: {
                string uval = unmarshallBinary(data);
                if (fromlevel == SEMANTIC_AGG) {
                    uint64_t val = decrypt_Paillier(uval);
                    fromlevel  = decreaseLevel(fromlevel, ft, oAGG);
                    if (fromlevel == tolevel) {
                        return marshallVal(val);
                    }
                }

                assert_s(false, "nothing lower than plain agg");

                return "";
            }
            default: {
                assert_s(false, "no other onions possible\n");
                return "";
            }
            }
            assert_s(false, "no other onions possible\n");
            return "";
        }
        case TYPE_TEXT: {

            switch (o) {
            case oDET: {

                string val = unmarshallBinary(data);
                if (fromlevel == SEMANTIC_DET) {
                    LOG(crypto) << "at sem det " << data;

                    AES_KEY * key =
                        get_key_SEM(getKey(mkey, fullfieldname, fromlevel));
                    val = decrypt_SEM(val, key, salt);
                    fromlevel  = decreaseLevel(fromlevel, ft, oDET);
                    if (fromlevel == tolevel) {
                        return marshallBinary(val);
                    }
                }

                if (fromlevel == DET) {
                    LOG(crypto) << "at det " << marshallBinary(val);

                    AES_KEY * key =
                        get_key_DET(getKey(mkey, fullfieldname, fromlevel));
                    val = decrypt_DET(val, key);
                    fromlevel = decreaseLevel(fromlevel, ft, oDET);
                    if (fromlevel == tolevel) {
                        return marshallBinary(val);
                    }
                }

                if (fromlevel == DETJOIN) {
                    LOG(crypto) << "at det join " << marshallBinary(val);

                    AES_KEY * key =
                        get_key_DET(getKey(mkey, "join", fromlevel));
                    val = decrypt_DET(val, key);
                    fromlevel = decreaseLevel(fromlevel, ft, oDET);
                    if (fromlevel == tolevel) {
                        LOG(crypto) << "at plain " << val;
                        return val;
                    }
                }

                assert_s(false, "nothing lower than plain");
                return "";
            }
            case oOPE: {

                uint64_t val = unmarshallVal(data);
                if (fromlevel == SEMANTIC_OPE) {
                    AES_KEY * key =
                        get_key_SEM(getKey(mkey, fullfieldname, fromlevel));
                    val = decrypt_SEM(val, key, salt);
                    fromlevel  = decreaseLevel(fromlevel, ft, oOPE);
                    if (fromlevel == tolevel) {
                        return marshallVal(val);
                    }
                }

                assert_s(
                    false,
                    "should not want to decrypt past OPESELF for text \n");

                return "";
            }
            default: {
                myassert(false, "no valid onion in text \n");
                return "";
            }

            }
            myassert(false, "no valid onion in text \n");
            return "";
        }
        default: {
            myassert(false, "no other types possible \n");
            return "";
        }
        }

    }

    //ENCRYPT
    myassert(comp < 0, "problem with crypt: comp should be > 0");

    switch (ft) {
    case TYPE_INTEGER: {

        switch (o) {
        case oDET: {

            uint64_t val = unmarshallVal(data);

            if (fromlevel == PLAIN_DET) {
                fromlevel = increaseLevel(fromlevel, ft, oDET);
                AES_KEY * key = get_key_DET(getKey(mkey, "join", fromlevel));
                val = encrypt_DET(val, key);
                if (fromlevel == tolevel) {
                    return marshallVal(val);
                }
            }

            if (fromlevel == DETJOIN) {
                fromlevel = increaseLevel(fromlevel, ft, oDET);
                AES_KEY * key =
                    get_key_DET(getKey(mkey, fullfieldname, fromlevel));
                val = encrypt_DET(val, key);
                if (fromlevel == tolevel) {
                    return marshallVal(val);
                }
            }

            if (fromlevel == DET) {
                fromlevel  = increaseLevel(fromlevel, ft, oDET);
                AES_KEY * key =
                    get_key_SEM(getKey(mkey, fullfieldname, fromlevel));
                val = encrypt_SEM(val, key, salt);
                if (fromlevel == tolevel) {
                    return marshallVal(val);
                }
            }

            assert_s(false, "nothing higher than SEM");

            return "";
        }
        case oOPE: {
            uint64_t val = unmarshallVal(data);

            if (fromlevel == PLAIN_OPE) {
                fromlevel = increaseLevel(fromlevel, ft, oOPE);
                if (fromlevel == tolevel) {
                    return marshallVal(val);
                }
            }

            if (fromlevel == OPEJOIN) {
                fromlevel = increaseLevel(fromlevel, ft, oOPE);
                OPE * key = get_key_OPE(getKey(mkey, fullfieldname, fromlevel));
                val = encrypt_OPE((uint32_t)val, key);
                if (fromlevel == tolevel) {
                    return marshallVal(val);
                }
            }

            if (fromlevel == OPESELF) {

                fromlevel  = increaseLevel(fromlevel, ft, oOPE);
                AES_KEY * key =
                    get_key_SEM(getKey(mkey, fullfieldname, fromlevel));
                val = encrypt_SEM(val, key, salt);
                if (fromlevel == tolevel) {
                    return marshallVal(val);
                }
            }

            assert_s(false, "nothing higher than OPE_SEM");

            return "";
        }
        case oAGG: {
            uint64_t val = unmarshallVal(data);
            if (fromlevel == PLAIN_AGG) {
                string uval = encrypt_Paillier(val);
                fromlevel = increaseLevel(fromlevel, ft, oAGG);
                if (fromlevel == tolevel) {
                    return marshallBinary(uval);
                }
            }

            assert_s(false, "nothing higher than sem agg");

            return "";
        }
        default: {
            assert_s(false, "no other onions possible\n");
            return "";
        }
        }
        assert_s(false, "no other onions possible\n");
        return "";
    }
    case TYPE_TEXT: {

        switch (o) {
        case oDET: {

            if (fromlevel == PLAIN_DET) {
                LOG(crypto) << "at plain det " << data;

                /* XXX
                 * This looks wrong: when do we put the apostrophe back?
                 */
                data = removeApostrophe(data);

                fromlevel  = increaseLevel(fromlevel, ft, oDET);

                AES_KEY * key =
                    get_key_DET(getKey(mkey, "join", fromlevel));

                data = encrypt_DET(data, key);
                //cerr << "crypting " << data << " at DET is " <<
                // marshallBinary(uval) << "  ";
                if (fromlevel == tolevel) {
                    //cerr << "result is " << marshallBinary(uval, newlen);
                    return marshallBinary(data);
                }

            } else {
                data = unmarshallBinary(data);
            }

            if (fromlevel == DETJOIN) {
                LOG(crypto) << "at det join " << marshallBinary(data);

                fromlevel = increaseLevel(fromlevel, ft, oDET);
                AES_KEY * key =
                    get_key_DET(getKey(mkey, fullfieldname, fromlevel));
                data = encrypt_DET(data, key);
                if (fromlevel == tolevel) {
                    return marshallBinary(data);
                }
            }

            if (fromlevel == DET) {
                LOG(crypto) << "at det " << marshallBinary(data);

                fromlevel = increaseLevel(fromlevel, ft, oDET);

                AES_KEY * key =
                    get_key_SEM(getKey(mkey, fullfieldname, fromlevel));
                data = encrypt_SEM(data, key, salt);
                if (fromlevel == tolevel) {
                    LOG(crypto) << "at sem " << marshallBinary(data);
                    return marshallBinary(data);
                }
            }

            assert_s(false, "nothing higher than SEM_DET for text\n");

            return "";
        }
        case oOPE: {
            uint64_t val;

            if (fromlevel == PLAIN_OPE) {
                data = removeApostrophe(data);
                fromlevel = increaseLevel(fromlevel, ft, oOPE);
                OPE * key = get_key_OPE(getKey(mkey, fullfieldname, fromlevel));
                val = encrypt_OPE_text_wrapper(data, key);
                if (fromlevel == tolevel) {
                    return marshallVal(val);
                }
            } else {
                val = unmarshallVal(data);
            }

            if (fromlevel == OPESELF) {
                fromlevel = increaseLevel(fromlevel, ft, oOPE);
                AES_KEY * key =
                    get_key_SEM(getKey(mkey, fullfieldname, fromlevel));
                val = encrypt_SEM(val, key, salt);
                if (fromlevel == tolevel) {
                    return marshallVal(val);
                }
            }

            assert_s(false, "nothing higher than OPE_SEM");

            return "";

        }
        default: {
            myassert(false, "no valid onion in text \n");
            return "";
        }
        }
        myassert(false, "no valid onion in text \n");
        return "";
    }
    default: {
        myassert(false, "no other types possible \n");
        return "";
    }
    }
    myassert(false, "no other types possible \n");
    return "";

}

uint64_t
CryptoManager::encrypt_OPE_onion(string uniqueFieldName, uint32_t value,
                                 uint64_t salt)
{
    uint64_t res = encrypt_OPE(value, uniqueFieldName);

    string key = getKey(uniqueFieldName, SEMANTIC_OPE);

    AES_KEY * aesKey = get_key_SEM(key);
    res = encrypt_SEM(res, aesKey, salt);

    return res;
}

uint64_t
CryptoManager::encrypt_DET_onion(string uniqueFieldName, uint32_t value,
                                 uint64_t salt)
{
    //cout << "KEY USED TO ENCRYPT field to JOINDET " << uniqueFieldName << "
    // " << marshallKey(getKey("join", DETJOIN)) << "\n"; fflush(stdout);

    string key = getKey("join", DETJOIN);
    AES_KEY * aesKey = get_key_DET(key);
    uint64_t res = encrypt_DET(value, aesKey);

    //cout << "join det enc is " << res << "\n";
    //cout << "KEY USED TO ENCRYPT field to DET " << uniqueFieldName << " " <<
    // marshallKey(getKey(uniqueFieldName, DET)) << "\n"; fflush(stdout);

    key = getKey(uniqueFieldName, DET);
    aesKey = get_key_DET(key);
    res =  encrypt_DET(res, aesKey);

    //cout << "det enc is " << res << "\n";

    //cout << "KEY USED TO ENCRYPT field to SEM " << uniqueFieldName << " " <<
    // marshallKey(getKey(uniqueFieldName, SEMANTIC)) << "\n"; fflush(stdout);

    key = getKey(uniqueFieldName, SEMANTIC_DET);
    aesKey = get_key_SEM(key);
    res = encrypt_SEM(res, aesKey, salt);

    return res;

}

string
CryptoManager::encrypt_text_DET_onion(string uniqueFieldName, string value,
                                      uint64_t salt)
{
    //cerr << "encrypting onion with fname " << uniqueFieldName.c_str() <<
    // "\n";

    string key = getKey(uniqueFieldName, DET);
    AES_KEY * aesKey = get_key_DET(key);

    string res = encrypt_DET_wrapper(value, aesKey);

    key = getKey(uniqueFieldName, SEMANTIC_DET);
    aesKey = get_key_SEM(key);
    return encrypt_SEM(res, aesKey, salt);
}

uint64_t
CryptoManager::encrypt_DET_onion(string uniqueFieldName, string value,
                                 uint64_t salt)
{
    string key = getKey(uniqueFieldName, DET);
    AES_KEY * aesKey = get_key_DET(key);
    uint64_t res =  encrypt_DET(unmarshallVal(value), aesKey);

    key = getKey(uniqueFieldName, SEMANTIC_DET);
    aesKey = get_key_SEM(key);
    res = encrypt_SEM(res, aesKey, salt);

    return res;

}

string assembleWords(list<string> * words);
list<string> * getWords(string text);

uint32_t
CryptoManager::encrypt_VAL(string uniqueFieldName, uint32_t value,
                           uint64_t salt)
{
    string key = getKey(uniqueFieldName, SEMANTIC_VAL);
    //cout << "key to encrypt " << uniqueFieldName << " is " <<
    // marshallKey(key) << "\n";
    AES_KEY * aesKey = get_key_SEM(key);
    //cout << "value is " << value << " encryption is " <<
    // marshallVal(encrypt_SEM(value, aesKey, salt)) << "\n";
    return encrypt_SEM(value, aesKey, salt);
}

string
CryptoManager::encrypt_VAL(string uniqueFieldName, string value,
                           uint64_t salt)
{
    string key = getKey(uniqueFieldName, SEMANTIC_VAL);
    AES_KEY * aesKey = get_key_SEM(key);
    return encrypt_SEM(value, aesKey, salt);
}

string
CryptoManager::getKey(const string &uniqueFieldName, SECLEVEL sec)
{
    return getKey(masterKey, uniqueFieldName, sec);
}

string
CryptoManager::getKey(AES_KEY * masterKeyArg, const string &uniqueFieldName,
                      SECLEVEL sec)
{
    string id = uniqueFieldName + marshallVal((unsigned int) sec);

    unsigned char shaDigest[SHA_DIGEST_LENGTH];
    SHA1((const uint8_t *) &id[0], id.length(), shaDigest);

    string result;
    result.resize(AES_BLOCK_BYTES);
    AES_encrypt(shaDigest, (uint8_t *) &result[0], masterKeyArg);
    return result;
}

string
CryptoManager::marshallKey(const string &key)
{
    // we will be sending key as two big nums
    string res = "";

    for (unsigned int i = 0; i < AES_KEY_SIZE/bitsPerByte; i++) {
        res = res + marshallVal((unsigned int)(key[i])) + ",";
    }

    //remove last comma
    res.resize(res.length() - 1);
    return res;
}

string
CryptoManager::unmarshallKey(const string &key)
{
    list<string> words = parse((key + '\0').c_str(),"", ", );", "");

    myassert(
        words.size() == AES_KEY_BYTES, "the given key string " + key +
        " is invalid");

    string reskey;
    reskey.resize(AES_KEY_BYTES);

    int i = 0;
    list<string>::iterator wordsIt = words.begin();

    while (wordsIt != words.end()) {
        uint64_t val = unmarshallVal(*wordsIt);
        myassert(val < 256,
                 "invalid key -- some elements are bigger than bytes " + key);
        reskey[i] = (unsigned char) (val % 256);
        wordsIt++; i++;
    }

    return reskey;
}

AES_KEY *
CryptoManager::get_key_SEM(const string &key)
{
    AES_KEY * aes_key = new AES_KEY();

    if (AES_set_encrypt_key((const uint8_t*) key.c_str(), AES_KEY_SIZE,
                            aes_key) <0) {
        myassert(false, "problem with AES set encrypt ");
    }

    return aes_key;

}

static uint64_t
getXORValue(uint64_t salt, AES_KEY * aes_key)
{
    string plaintext = BytesFromInt(salt, AES_BLOCK_BYTES);
    unsigned char ciphertext[AES_BLOCK_BYTES];
    AES_encrypt((const uint8_t*)plaintext.c_str(), ciphertext, aes_key);

    return IntFromBytes(ciphertext, AES_BLOCK_BYTES);
}

uint64_t
CryptoManager::encrypt_SEM(uint64_t ptext, AES_KEY * key, uint64_t salt)
{
    return ptext ^ getXORValue(salt, key);

}

uint64_t
CryptoManager::decrypt_SEM(uint64_t ctext, AES_KEY * key, uint64_t salt)
{
    return ctext ^ getXORValue(salt, key);
}

uint32_t
CryptoManager::encrypt_SEM(uint32_t ptext, AES_KEY * key, uint64_t salt)
{
    return ptext ^ (uint32_t) getXORValue(salt, key);
}

uint32_t
CryptoManager::decrypt_SEM(uint32_t ctext, AES_KEY * key, uint64_t salt)
{
    return ctext ^ (uint32_t) getXORValue(salt, key);
}

static vector<unsigned char>
getXorVector(size_t len, AES_KEY * key, uint64_t salt)
{
    size_t AESBlocks = len / AES_BLOCK_BYTES;
    if (AESBlocks * AES_BLOCK_BYTES < len)
        AESBlocks++;

    //construct vector with which we will XOR
    vector<unsigned char> v(AESBlocks * AES_BLOCK_BYTES);

    for (unsigned int i = 0; i < AESBlocks; i++)
        AES_encrypt((const uint8_t*) BytesFromInt(salt+i,
                                                  AES_BLOCK_BYTES).c_str(),
                    &v[i*AES_BLOCK_BYTES], key);

    return v;
}

string
CryptoManager::encrypt_SEM(const string &ptext, AES_KEY * key, uint64_t salt)
{
    vector<unsigned char> xorVector = getXorVector(ptext.length(), key, salt);

    stringstream ss;
    for (unsigned int i = 0; i < ptext.length(); i++)
        ss << (uint8_t) (((uint8_t)ptext[i]) ^ xorVector[i]);

    return ss.str();
}

string
CryptoManager::decrypt_SEM(const string &ctext, AES_KEY * key, uint64_t salt)
{
    vector<unsigned char> xorVector = getXorVector(ctext.length(), key, salt);

    stringstream ss;
    for (unsigned int i = 0; i < ctext.length(); i++)
        ss << (uint8_t) (((uint8_t)ctext[i]) ^ xorVector[i]);

    return ss.str();
}

string
CryptoManager::encrypt_DET(const string & ptext, AES_KEY * key)
{
    return CryptoManager::encrypt_SEM(ptext, key, 1);
}

string
CryptoManager::decrypt_DET(const string & ctext, AES_KEY * key)
{
    return CryptoManager::decrypt_SEM(ctext, key, 1);
}

void
CryptoManager::setMasterKey(const string &masterKeyArg)
{
    masterKey = new AES_KEY();

    AES_set_encrypt_key(
        (const uint8_t *) masterKeyArg.c_str(), AES_KEY_SIZE, masterKey);

    RAND_seed((const uint8_t *) masterKeyArg.c_str(), MASTER_KEY_SIZE);

    SetSeed(ZZFromBytes((const uint8_t *) masterKeyArg.c_str(),
                        MASTER_KEY_SIZE));
}

/*
   string CryptoManager::decrypt_SEM_toString(unsigned char * etext, unsigned
      int elen, AES_KEY * key, uint64_t salt) {

    unsigned char * xorVector = getXorVector(elen, key, salt);

    unsigned char c;
    string result = "";
    for (unsigned int i = 0; i < elen; i++) {
        c = etext[i] ^ xorVector[i];
        myassert(isprint(c), "decrypt SEM failed -- non readable
           characters");
        result = result + (char)(c);
    }

    return result;

   }
 */

OPE *
CryptoManager::get_key_OPE(const string &key)
{
    return new OPE(key, OPE_PLAINTEXT_SIZE, OPE_CIPHERTEXT_SIZE);
}

string
CryptoManager::encrypt_OPE(const string &plaintext, OPE * ope)
{
    //return randomBytes(OPE_PLAINTEXT_SIZE);
    //cerr << "ope!\n";
    return ope->encrypt(plaintext);
}

uint64_t
CryptoManager::encrypt_OPE_text_wrapper(const string & plaintext, OPE * ope)
{
    size_t len = plaintext.length();
    size_t prefix = OPE_PLAINTEXT_SIZE/bitsPerByte;
    size_t mins = min(prefix, len);

    LOG(crypto) << "mins is " << mins;

    string p2 = toLowerCase(plaintext.substr(0, mins));

    uint32_t val = 0;
    for (unsigned i = 0; i < mins; i++) {
        val = val*256 + (unsigned int)p2[i];
    }

    for (unsigned int i = 0; i < prefix - mins; i++) {
        val = val * 256;
    }

    LOG(crypto) << "for string " << plaintext << " encrypted val is " << val;

    return ope->encrypt(val);
}

string
CryptoManager::decrypt_OPE(const string &ciphertext, OPE * ope)
{

    //cerr << "ope!\n";
    return ope->decrypt(ciphertext);
}

uint64_t
CryptoManager::encrypt_OPE(uint32_t plaintext, OPE * ope)
{
    //return 3;
    //cerr << "ope!\n";
    return ope->encrypt(plaintext);
}

uint32_t
CryptoManager::decrypt_OPE(uint64_t ciphertext, OPE * ope)
{
    //cerr << "ope!\n";
    return ope->decrypt(ciphertext);
}

uint64_t
CryptoManager::encrypt_OPE(uint32_t plaintext, string uniqueFieldName)
{
    //cerr << "ope!\n";
    if (useEncTables) {
        map<string, map<int, uint64_t> >::iterator it = OPEEncTable.find(
            uniqueFieldName);
        assert_s(it != OPEEncTable.end(),
                 string(
                     " there should be entry in OPEEncTables for ") +
                 uniqueFieldName );
        map<int, uint64_t>::iterator sit = it->second.find(plaintext);
        if (sit != it->second.end()) {
            LOG(crypto_v) << "OPE hit for " << plaintext;
            return sit->second;
        }
        LOG(crypto_v) << "OPE miss for " << plaintext;
    }

    return encrypt_OPE(plaintext, get_key_OPE(getKey(uniqueFieldName, OPESELF)));
}

AES_KEY *
CryptoManager::get_key_DET(const string &key)
{
    AES_KEY * aes_key = new AES_KEY();

    if (AES_set_encrypt_key((const uint8_t *) key.c_str(), AES_KEY_SIZE,
                            aes_key) <0) {
        myassert(false, "problem with AES set encrypt ");
    }
    return aes_key;

}

/*
   AES_KEY * CryptoManager::get_dkey_DET(unsigned char * key) {
        myassert(key!=NULL, "given key is null");
        AES_KEY * aes_key = new AES_KEY();

        if (AES_set_decrypt_key(key, AES_KEY_SIZE, aes_key) <0) {
                myassert(false, "problem with AES set decrypt ");
        }
        return aes_key;

   }
 */
//TODO: this needs to be fixed, perhaps use evp
uint64_t
CryptoManager::encrypt_DET(uint64_t plaintext, AES_KEY * key)
{

    return encrypt_SEM(plaintext, key, 1);
    /*
       unsigned char * plainBytes = BytesFromInt(plaintext, AES_BLOCK_BYTES);
       unsigned char * ciphertext = new unsigned char[AES_BLOCK_BYTES];
       AES_encrypt(plainBytes, ciphertext, key);

       return IntFromBytes(ciphertext, AES_BLOCK_BYTES);
     */
}

uint64_t
CryptoManager::decrypt_DET(uint64_t ciphertext, AES_KEY * key)
{

    return decrypt_SEM(ciphertext, key, 1);
    /*
       unsigned char * ciphBytes = BytesFromInt(ciphertext, AES_BLOCK_BYTES);
       unsigned char * plaintext = new unsigned char[AES_BLOCK_BYTES];
       AES_decrypt(ciphBytes, plaintext, key);

       return IntFromBytes(plaintext, AES_BLOCK_BYTES);
     */
}

uint64_t
CryptoManager::encrypt_DET(uint32_t plaintext, AES_KEY * key)
{

    return encrypt_SEM((uint64_t) plaintext, key, 1);

    /*
       unsigned char * plainBytes = BytesFromInt((uint64_t)plaintext,
          AES_BLOCK_BYTES);
       unsigned char * ciphertext = new unsigned char[AES_BLOCK_BYTES];
       AES_encrypt(plainBytes, ciphertext, key);

       cout << "to encrypt for JOIN <" << plaintext << "> result is " <<
          IntFromBytes(ciphertext, AES_BLOCK_BYTES) << "\n"; fflush(stdout);

       return IntFromBytes(ciphertext, AES_BLOCK_BYTES);*/

}
/*
   //AES_K(hash(test))
   uint64_t
   CryptoManager::encrypt_DET(const string &plaintext, AES_KEY *key)
   {
    unsigned int plainLen = plaintext.size();
    unsigned char * plainBytes = (unsigned char*) plaintext.c_str();

    unsigned char shaDigest[SHA_DIGEST_LENGTH];
    SHA1(plainBytes, plainLen, shaDigest);

    unsigned char ciphertext[AES_BLOCK_BYTES];
    AES_encrypt(shaDigest, ciphertext, key);

    return IntFromBytes(ciphertext, AES_BLOCK_BYTES);
   }
 */

static void
xorWord(string word, AES_KEY * key, int salt, stringstream *ss)
{
    size_t plen = word.length();
    vector<unsigned char> xorVector = getXorVector(plen, key, salt);

    for (unsigned int i = 0; i < plen; i++)
        (*ss) << (unsigned char) (((unsigned char)word[i]) ^ xorVector[i]);
}

static string
unxorWord(AES_KEY * key, int salt, const string &s)
{
    vector<unsigned char> xorVector = getXorVector(s.length(), key, salt);

    stringstream ss;
    for (unsigned int i = 0; i < s.length(); i++)
        ss << (char) (((uint8_t)s[i]) ^ xorVector[i]);

    return ss.str();
}

string
CryptoManager::encrypt_DET_search(list<string> * words, AES_KEY * key)
{
    stringstream ss;

    int index = 0;
    for (auto it = words->begin(); it != words->end(); it++) {
        //cerr << "word len is " << it->length() << "\n";
        if (it->length() > 255) {*it = it->substr(0, 254); }
        ss << (uint8_t) it->length();

        xorWord(*it, key, index, &ss);
        index++;
    }

    //cerr << "total len is " << len << " CIPH after enc "; myPrint(ciph,
    // len); cerr << "\n";
    return ss.str();
}

list<string> *
CryptoManager::decrypt_DET_search(const string &ctext, AES_KEY * key)
{
    //cerr << "CIPH to decrypt " ; myPrint(ciph, len); cerr << "\n";

    unsigned int pos = 0;
    int index = 0;
    list<string> * res = new list<string>();

    while (pos < ctext.length()) {
        uint wlen = (uint8_t) ctext[pos];
        //cerr << "wlen is " << wlen << "\n";
        pos++;

        res->push_back(unxorWord(key, index, string(&ctext[pos], wlen)));
        index++;
        pos = pos + wlen;
    }

    return res;
}

//returns the concatenation of all words in the given list
string
assembleWords(list<string> * words)
{
    string res = "";

    for (list<string>::iterator it = words->begin(); it != words->end();
         it++) {
        res = res + *it;
    }

    return res;
}

//returns a list of words and separators
list<string> *
getWords(string text)
{
    list<string> * words =  new list<string>;

    size_t len = text.length();

    for (unsigned int pos = 0; pos < len; )
    {
        string word = "";
        while (pos < len && wordSeparators.find(text[pos]) ==
               string::npos) {
            word = word + text[pos];
            pos++;
        }
        if (word.length() > 0) {
            words->push_back(word);
        }

        string sep = "";
        while (pos < len && wordSeparators.find(text[pos]) !=
               string::npos) {
            sep = sep + text[pos];
            pos++;
        }
        myassert(pos == len || sep.length() > 0, "error");

        if (sep.length() > 0) {
            words->push_back(sep);
        }

    }

    return words;
}

string
CryptoManager::encrypt_DET_wrapper(const string &text, AES_KEY * key)
{
    return CryptoManager::encrypt_DET_search(getWords(text), key);
}

string
CryptoManager::decrypt_DET_wrapper(const string &ctext, AES_KEY * key)
{
    return assembleWords(CryptoManager::decrypt_DET_search(ctext, key));
}

string
CryptoManager::encrypt_Paillier(uint64_t val)
{
    //cerr << "paillier!\n";
    if (useEncTables) {
        auto it = HOMEncTable.find(val);
        if (it != HOMEncTable.end()) {
            if (it->second.size() > 0) {
                string res = it->second.front();
                it->second.pop_front();
                LOG(crypto_v) << "HOM hit for " << val;
                return res;
            }
        }

        LOG(crypto_v) << "HOM miss for " << val;
    }

    ZZ r = RandomLen_ZZ(Paillier_len_bits/2) % Paillier_n;
    //myassert(Paillier_g < Paillier_n2, "error: g > n2!");
    ZZ c = PowerMod(Paillier_g, to_ZZ(val) + Paillier_n*r, Paillier_n2);

    //cerr << "Paillier encryption is " << c << "\n";
    return StringFromZZ(c);
}

int
CryptoManager::decrypt_Paillier(const string &ciphertext)
{
    //cerr << "paillier!\n";
    //cerr << "to Paillier decrypt "; myPrint(ciphertext, Paillier_len_bytes);
    // cerr << "\n";
    //cerr << "N2 is " << Paillier_n2 << "\n";
    ZZ c = ZZFromBytes((uint8_t*)ciphertext.c_str(), Paillier_len_bytes);
    //cerr << "zz to decrypt " << c << "\n";
    //myassert(c < Paillier_n2, "error: c > Paillier_n2");
    ZZ m =
        MulMod(  Paillier_L(PowerMod(c, Paillier_lambda,
                                     Paillier_n2), Paillier_n),
                 Paillier_dec_denom,
                 Paillier_n);

    //cerr << "Paillier N2 is " << Paillier_n2 << "\n";
    //cerr << "Paillier N2 in bytes is "; myPrint(BytesFromZZ(Paillier_n2,
    // Paillier_len_bytes), Paillier_len_bytes); cerr << "\n";
    //cerr << "result is " << m << "\n";
    return to_int(m);
}

string
CryptoManager::getPKInfo()
{
    return StringFromZZ(Paillier_n2);
}

void
CryptoManager::createEncryptionTables(int noOPEarg, int noHOMarg,
                                      list<string>  fieldsWithOPE)
{

    int encryptionsOfOne = 100;
    int noEncryptions = 5;

    noOPE = noOPEarg;
    noHOM = noHOMarg;

    OPEEncTable.clear();
    HOMEncTable.clear();

    struct timeval starttime, endtime;
    //OPE

    gettimeofday(&starttime, NULL);

    for (list<string>::iterator it = fieldsWithOPE.begin();
         it != fieldsWithOPE.end(); it++) {
        string anonName = *it;
        OPEEncTable[anonName] = map<int, uint64_t>();
        OPE * currentKey = get_key_OPE(getKey(anonName, OPESELF));
        for (int i = 0; i < noOPE; i++) {
            OPEEncTable[anonName][i] = encrypt_OPE(i, currentKey);
        }

    }
    gettimeofday(&endtime, NULL);
    LOG(crypto) << "time per OPE "
                << timeInSec(starttime, endtime) * 1000.0 / noOPE;

    gettimeofday(&starttime, NULL);
    // HOM
    for (int i = 0; i < encryptionsOfOne; i++) {
        HOMEncTable[1] = list<string>();
        HOMEncTable[1].push_back(encrypt_Paillier(1));
    }

    for (int i = 0; i < noHOM; i++) {
        if (i != 1) {
            HOMEncTable[i] = list<string>();
            for (int j = 0; j < noEncryptions; j++) {
                HOMEncTable[i].push_back(encrypt_Paillier(i));
            }
        }
    }

    gettimeofday(&endtime, NULL);
    LOG(crypto) << "per HOM "
                << timeInSec(starttime, endtime)*1000.0 /
                   (encryptionsOfOne + noHOM * noEncryptions);

    LOG(crypto) << "entries in OPE table are:";
    for (map<string, map<int, uint64_t> >::iterator it = OPEEncTable.begin();
         it != OPEEncTable.end(); it++) {
        LOG(crypto) << it->first;
    }

    LOG(crypto) << "entries for HOM are:";
    for (auto it = HOMEncTable.begin(); it != HOMEncTable.end(); it++) {
        LOG(crypto) << it->first;
    }

    useEncTables = true;
}

void
CryptoManager::replenishEncryptionTables()
{
    assert_s(false, "unimplemented replenish");
}

//**************** Public Key Cryptosystem (PKCS)
// ****************************************/

//marshall key
static string
DER_encode_RSA_public(RSA *rsa)
{
    string s;
    s.resize(i2d_RSAPublicKey(rsa, 0));

    uint8_t *next = (uint8_t *) &s[0];
    i2d_RSAPublicKey(rsa, &next);
    return s;
}

static RSA *
DER_decode_RSA_public(const string &s)
{
    const uint8_t *buf = (const uint8_t*) s.c_str();
    return d2i_RSAPublicKey(0, &buf, s.length());
}

//marshall key
static string
DER_encode_RSA_private(RSA *rsa)
{
    string s;
    s.resize(i2d_RSAPrivateKey(rsa, 0));

    uint8_t *next = (uint8_t *) &s[0];
    i2d_RSAPrivateKey(rsa, &next);
    return s;
}

static RSA *
DER_decode_RSA_private(const string &s)
{
    const uint8_t *buf = (const uint8_t*) s.c_str();
    return d2i_RSAPrivateKey(0, &buf, s.length());
}

static void
remove_private_key(RSA *r)
{
    r->d = r->p = r->q = r->dmp1 = r->dmq1 = r->iqmp = 0;
}

//Credits: the above five functions are from "secure programming cookbook for
// C++"

void
CryptoManager::generateKeys(PKCS * & pk, PKCS * & sk)
{
    LOG(crypto) << "pkcs generate";
    PKCS * key =  RSA_generate_key(PKCS_bytes_size*8, 3, NULL, NULL);

    sk = RSAPrivateKey_dup(key);

    pk = key;
    remove_private_key(pk);

}

string
CryptoManager::marshallKey(PKCS * mkey, bool ispk)
{
    LOG(crypto) << "pkcs encrypt";
    string key;
    if (!ispk) {
        key = DER_encode_RSA_private(mkey);
    } else {
        key = DER_encode_RSA_public(mkey);
    }
    assert_s(key.length() >= 1, "issue with RSA pk \n");
    return key;
}

PKCS *
CryptoManager::unmarshallKey(const string &key, bool ispk)
{
    LOG(crypto) << "pkcs decrypt";
    //cerr << "before \n";
    if (ispk) {
        return DER_decode_RSA_public(key);
    } else {
        return DER_decode_RSA_private(key);
    }
}

string
CryptoManager::encrypt(PKCS * key, const string &s)
{
    string tocipher;
    tocipher.resize(RSA_size(key));

    RSA_public_encrypt((int) s.length(),
                       (const uint8_t*) s.c_str(), (uint8_t*) &tocipher[0],
                       key,
                       RSA_PKCS1_OAEP_PADDING);

    return tocipher;
}

string
CryptoManager::decrypt(PKCS * key, const string &s)
{
    assert_s(s.length() == (uint)RSA_size(key), "fromlen is not RSA_size");
    string toplain;
    toplain.resize(RSA_size(key));

    uint len =
        RSA_private_decrypt((int) s.length(),
                            (const uint8_t*) s.c_str(),
                            (uint8_t*) &toplain[0], key,
                            RSA_PKCS1_OAEP_PADDING);
    toplain.resize(len);

    return toplain;
}

void
CryptoManager::freeKey(PKCS * key)
{
    RSA_free(key);
}

//***************************************************************************************/

CryptoManager::~CryptoManager()
{
    free(masterKey);

    map<string, map<int, uint64_t> >::iterator it = OPEEncTable.begin();

    for (; it != OPEEncTable.end(); it++) {
        it->second.clear();
    }

    OPEEncTable.clear();

    auto homit = HOMEncTable.begin();
    for (; homit != HOMEncTable.end(); homit++) {
        homit->second.clear();
    }

    HOMEncTable.clear();

}
