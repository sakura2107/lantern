package org.lantern.client;
import android.content.Context;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import javax.crypto.*;
import javax.crypto.spec.GCMParameterSpec;
final class Secrets {
    static javax.crypto.SecretKey key() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (!store.containsAlias("lantern-pairing")) {
            KeyGenerator generator =
                KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            generator.init(
                new KeyGenParameterSpec
                    .Builder("lantern-pairing", KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .build());
            return generator.generateKey();
        }
        return (javax.crypto.SecretKey)store.getKey("lantern-pairing", null);
    }
    /** {iv, ciphertext}, both Base64, sealed with the app's Keystore key. */
    static String[] encrypt(String token) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key());
        return new String[] {Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP),
                             Base64.encodeToString(cipher.doFinal(token.getBytes(StandardCharsets.UTF_8)), Base64.NO_WRAP)};
    }
    static String decrypt(String iv, String data) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(128, Base64.decode(iv, Base64.NO_WRAP)));
        return new String(cipher.doFinal(Base64.decode(data, Base64.NO_WRAP)), StandardCharsets.UTF_8);
    }
    static void save(Context context, String token) throws Exception {
        String[] sealed = encrypt(token);
        context.getSharedPreferences("secret", 0)
            .edit()
            .putString("iv", sealed[0])
            .putString("cipher", sealed[1])
            .apply();
    }
    static String load(Context context) throws Exception {
        var p = context.getSharedPreferences("secret", 0);
        if (!p.contains("iv"))
            return "";
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key(),
                    new GCMParameterSpec(128, Base64.decode(p.getString("iv", ""), Base64.NO_WRAP)));
        return new String(cipher.doFinal(Base64.decode(p.getString("cipher", ""), Base64.NO_WRAP)),
                          StandardCharsets.UTF_8);
    }
}
