/*
 * ECJoin.cpp
 *
 */

#include "ECJoin.h"


EC_POINT *
ECJoin::randomPoint() {
    EC_POINT * point = EC_POINT_new(group);

    BIGNUM *x = BN_new(), *y = BN_new();

    bool found = false;

    while (!found) {
        BN_rand_range(x, order);
        assert_s(EC_POINT_set_compressed_coordinates_GFp(group, point, x, 1, NULL), "issue setting coordinates");
        assert_s(EC_POINT_get_affine_coordinates_GFp(group, point, x, y, NULL),"issue getting coordinates");

        if(BN_is_zero(x) || BN_is_zero(y)) {
                found = false;
                continue;
        }

        if(EC_POINT_is_on_curve(group, point, NULL)) {
            found = true;
        }
    }

    BN_free(x);
    BN_free(y);
    return point;
}

ECJoin::ECJoin()
{

    group = EC_GROUP_new_by_curve_name(NID);
    assert_s(group, "issue creating new curve");

    order = BN_new();

    assert_s(EC_GROUP_get_order(group, order, NULL), "failed to retrieve the order");

    P = randomPoint();

    Infty = EC_POINT_new(group);
    assert_s(Infty, "failed to create point at infinity");

    ZeroBN = BN_new();
    assert_s(ZeroBN != NULL, "cannot create big num");
    BN_zero(ZeroBN);

}

static EC_POINT *
mul(EC_GROUP * group, BIGNUM * ZeroBN, EC_POINT * Point, BIGNUM * Scalar) {

    EC_POINT * ans = EC_POINT_new(group);
    assert_s(ans, "cannot create point ");

    //ans = sk->kp * cbn
    assert_s(EC_POINT_mul(group, ans, ZeroBN, Point, Scalar, NULL), "issue when multiplying ec");

    return ans;
}


ECJoinSK *
ECJoin::getSKey(const string & key) {
    ECJoinSK * skey = new ECJoinSK();
    skey->aesKey = get_AES_KEY(key);

    skey->k = BN_bin2bn((unsigned char *)key.c_str(), key.length(), NULL);

    assert_s(skey->k != NULL, "failed to convert key to BIGNUM");

    skey->kP = mul(group, ZeroBN, P, skey->k);

    return skey;
}

ECDeltaSK *
ECJoin::getDeltaKey(const ECJoinSK * key1, const ECJoinSK *  key2) {
    ECDeltaSK * delta = new ECDeltaSK();

    delta->group = group;

    BIGNUM * key1Inverse = BN_mod_inverse(NULL, key1->k, order, NULL);
    assert_s(key1Inverse, "could not compute inverse of key 1");

    delta->deltaK = BN_new();
    BN_mod_mul(delta->deltaK, key1Inverse, key2->k, order, NULL);
    assert_s(delta->deltaK, "failed to multiply");

    delta->ZeroBN = ZeroBN;

    BN_free(key1Inverse);

    return delta;

}

// a PRF with 128 bits security, but 160 bit output
string
ECJoin::PRFForEC(const AES_KEY * sk, const string & ptext) {

    string nptext = ptext;

    unsigned int len = ptext.length();

    if (bytesLong > len) {
        for (unsigned int i = 0 ; i < bytesLong - len; i++) {
            nptext = nptext + "0";
        }
    }

    return encrypt_AES(nptext, sk, 1).substr(0, bytesLong);

}

string
ECJoin::point2Str(EC_GROUP * group, EC_POINT * point) {
    unsigned char buf[ECJoin::MAX_BUF];
    memset(buf, 0, ECJoin::MAX_BUF);

    size_t len = 0;

    len = EC_POINT_point2oct(group, point, POINT_CONVERSION_COMPRESSED, buf, len, NULL);

    assert_s(len, "cannot serialize EC_POINT ");

    return string((char *)buf, len);
}

string
ECJoin::encrypt(ECJoinSK * sk, const string & ptext) {

    // CONVERT ptext in PRF(ptext)
    string ctext = PRFForEC(sk->aesKey, ptext);

    //cbn = PRF(ptext)
    BIGNUM * cbn = BN_bin2bn((const unsigned char *)ctext.c_str(), ctext.length(), NULL);
    assert_s(cbn, "issue convering string to BIGNUM ");

    //ans = sk->kp * cbn
    EC_POINT * ans = mul(group, ZeroBN, sk->kP, cbn);

    string res = point2Str(group, ans);

    EC_POINT_free(ans);
    BN_free(cbn);

    return res;
}

string
ECJoin::adjust(ECDeltaSK * delta, const string & ctext) {

    EC_POINT * point = EC_POINT_new(delta->group);

    assert_s(EC_POINT_oct2point(delta->group, point, (const unsigned char *)ctext.c_str(), ctext.length(), NULL),
            "cannot convert from ciphertext to point");

    EC_POINT * res = mul(delta->group, delta->ZeroBN, point, delta->deltaK);

    string result = point2Str(delta->group, res);

    EC_POINT_free(res);
    EC_POINT_free(point);

    return result;
}

ECJoin::~ECJoin()
{
    BN_free(order);
    EC_POINT_free(P);
    EC_GROUP_clear_free(group);
}
