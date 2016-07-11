/*
  * Copyright 2010-2016 Amazon.com, Inc. or its affiliates. All Rights Reserved.
  * 
  * Licensed under the Apache License, Version 2.0 (the "License").
  * You may not use this file except in compliance with the License.
  * A copy of the License is located at
  * 
  *  http://aws.amazon.com/apache2.0
  * 
  * or in the "license" file accompanying this file. This file is distributed
  * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
  * express or implied. See the License for the specific language governing
  * permissions and limitations under the License.
  */

#include <cstring>

#include <aws/core/utils/crypto/openssl/CryptoImpl.h>
#include <aws/core/utils/Outcome.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/err.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <thread>

using namespace Aws::Utils;
using namespace Aws::Utils::Crypto;

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            namespace OpenSSL
            {
                static const char* OPENSSL_INTERNALS_TAG = "OpenSSLCallbackState";
                static std::mutex* locks(nullptr);

                GetTheLights getTheLights;

                void init_static_state()
                {
                    ERR_load_CRYPTO_strings();
                    OPENSSL_add_all_algorithms_noconf();

                    if (!CRYPTO_get_locking_callback())
                    {
                        locks = Aws::NewArray<std::mutex>(static_cast<size_t>(CRYPTO_num_locks()),
                                                          OPENSSL_INTERNALS_TAG);
                        CRYPTO_set_locking_callback(&locking_fn);
                    }

                    if (!CRYPTO_get_id_callback())
                    {
                        CRYPTO_set_id_callback(&id_fn);
                    }

                    RAND_poll();
                }

                void cleanup_static_state()
                {
                    if (CRYPTO_get_locking_callback() == &locking_fn)
                    {
                        CRYPTO_set_id_callback(nullptr);
                        assert(locks);
                        Aws::DeleteArray(locks);
                        locks = nullptr;
                    }

                    if (CRYPTO_get_id_callback() == &id_fn)
                    {
                        CRYPTO_set_locking_callback(nullptr);
                    }
                }

                void locking_fn(int mode, int n, const char*, int)
                {
                    if (mode & CRYPTO_LOCK)
                    {
                        locks[n].lock();
                    }
                    else
                    {
                        locks[n].unlock();
                    }
                }

                unsigned long id_fn()
                {
                    return static_cast<unsigned long>(std::hash<std::thread::id>()(std::this_thread::get_id()));
                }
            }

            void SecureRandomBytes_OpenSSLImpl::GetBytes(unsigned char* buffer, size_t bufferSize)
            {
                assert(buffer);

                int success = RAND_bytes(buffer, static_cast<int>(bufferSize));
                if (success != 1)
                {
                    m_failure = true;
                }
            }

            HashResult MD5OpenSSLImpl::Calculate(const Aws::String& str)
            {
                MD5_CTX md5;
                MD5_Init(&md5);
                MD5_Update(&md5, str.c_str(), str.size());

                ByteBuffer hash(MD5_DIGEST_LENGTH);
                MD5_Final(hash.GetUnderlyingData(), &md5);

                return HashResult(std::move(hash));
            }

            HashResult MD5OpenSSLImpl::Calculate(Aws::IStream& stream)
            {
                MD5_CTX md5;
                MD5_Init(&md5);

                auto currentPos = stream.tellg();
                if(currentPos == -1)
                {
                    currentPos = 0;
                    stream.clear();
                }
                stream.seekg(0, stream.beg);

                char streamBuffer[Aws::Utils::Crypto::Hash::INTERNAL_HASH_STREAM_BUFFER_SIZE];
                while (stream.good())
                {
                    stream.read(streamBuffer, Aws::Utils::Crypto::Hash::INTERNAL_HASH_STREAM_BUFFER_SIZE);
                    auto bytesRead = stream.gcount();

                    if (bytesRead > 0)
                    {
                        MD5_Update(&md5, streamBuffer, static_cast<size_t>(bytesRead));
                    }
                }

                stream.clear();
                stream.seekg(currentPos, stream.beg);

                ByteBuffer hash(MD5_DIGEST_LENGTH);
                MD5_Final(hash.GetUnderlyingData(), &md5);

                return HashResult(std::move(hash));
            }

            HashResult Sha256OpenSSLImpl::Calculate(const Aws::String& str)
            {
                SHA256_CTX sha256;
                SHA256_Init(&sha256);
                SHA256_Update(&sha256, str.c_str(), str.size());

                ByteBuffer hash(SHA256_DIGEST_LENGTH);
                SHA256_Final(hash.GetUnderlyingData(), &sha256);

                return HashResult(std::move(hash));
            }

            HashResult Sha256OpenSSLImpl::Calculate(Aws::IStream& stream)
            {
                SHA256_CTX sha256;
                SHA256_Init(&sha256);

                auto currentPos = stream.tellg();
                if(currentPos == -1)
                {
                    currentPos = 0;
                    stream.clear();
                }

                stream.seekg(0, stream.beg);

                char streamBuffer[Aws::Utils::Crypto::Hash::INTERNAL_HASH_STREAM_BUFFER_SIZE];
                while (stream.good())
                {
                    stream.read(streamBuffer, Aws::Utils::Crypto::Hash::INTERNAL_HASH_STREAM_BUFFER_SIZE);
                    auto bytesRead = stream.gcount();

                    if (bytesRead > 0)
                    {
                        SHA256_Update(&sha256, streamBuffer, static_cast<size_t>(bytesRead));
                    }
                }

                stream.clear();
                stream.seekg(currentPos, stream.beg);

                ByteBuffer hash(SHA256_DIGEST_LENGTH);
                SHA256_Final(hash.GetUnderlyingData(), &sha256);

                return HashResult(std::move(hash));
            }

            HashResult Sha256HMACOpenSSLImpl::Calculate(const ByteBuffer& toSign, const ByteBuffer& secret)
            {
                unsigned int length = SHA256_DIGEST_LENGTH;
                ByteBuffer digest(length);
                memset(digest.GetUnderlyingData(), 0, length);

                HMAC_CTX ctx;
                HMAC_CTX_init(&ctx);

                HMAC_Init_ex(&ctx, secret.GetUnderlyingData(), static_cast<int>(secret.GetLength()), EVP_sha256(),
                             NULL);
                HMAC_Update(&ctx, toSign.GetUnderlyingData(), toSign.GetLength());
                HMAC_Final(&ctx, digest.GetUnderlyingData(), &length);
                HMAC_CTX_cleanup(&ctx);

                return HashResult(std::move(digest));
            }

            static const char* OPENSSL_LOG_TAG = "OpenSSLCipher";

            void LogErrors(const char* logTag = OPENSSL_LOG_TAG)
            {
                unsigned long errorCode = ERR_get_error();
                char errStr[256];
                ERR_error_string_n(errorCode, errStr, 256);

                AWS_LOGSTREAM_ERROR(logTag, errStr);
            }


            OpenSSLCipher::OpenSSLCipher(const CryptoBuffer& key, size_t blockSizeBytes, bool ctrMode) :
                    SymmetricCipher(key, blockSizeBytes, ctrMode), m_encDecInitialized(false), m_encryptionMode(false),
                    m_decryptionMode(false)
            {
                Init();
            }

            OpenSSLCipher::OpenSSLCipher(OpenSSLCipher&& toMove) : SymmetricCipher(std::move(toMove)),
                                                                   m_encDecInitialized(false)
            {
                m_ctx = toMove.m_ctx;
                toMove.m_ctx.cipher = nullptr;
                toMove.m_ctx.cipher_data = nullptr;
                toMove.m_ctx.engine = nullptr;

                m_encDecInitialized = toMove.m_encDecInitialized;
                m_encryptionMode = toMove.m_encryptionMode;
                m_decryptionMode = toMove.m_decryptionMode;
            }

            OpenSSLCipher::OpenSSLCipher(CryptoBuffer&& key, CryptoBuffer&& initializationVector, CryptoBuffer&& tag) :
                    SymmetricCipher(std::move(key), std::move(initializationVector), std::move(tag)),
                    m_encDecInitialized(false),
                    m_encryptionMode(false), m_decryptionMode(false)
            {
                Init();
            }

            OpenSSLCipher::OpenSSLCipher(const CryptoBuffer& key, const CryptoBuffer& initializationVector,
                                         const CryptoBuffer& tag) :
                    SymmetricCipher(key, initializationVector, tag), m_encDecInitialized(false),
                    m_encryptionMode(false), m_decryptionMode(false)
            {
                Init();
            }

            OpenSSLCipher::~OpenSSLCipher()
            {
                Cleanup();
            }

            void OpenSSLCipher::Init()
            {
                EVP_CIPHER_CTX_init(&m_ctx);
            }

            void OpenSSLCipher::CheckInitEncryptor()
            {
                assert(!m_failure);
                assert(!m_decryptionMode);

                if (!m_encDecInitialized)
                {
                    InitEncryptor_Internal();
                    m_encryptionMode = true;
                    m_encDecInitialized = true;
                }
            }

            void OpenSSLCipher::CheckInitDecryptor()
            {
                assert(!m_failure);
                assert(!m_encryptionMode);

                if (!m_encDecInitialized)
                {
                    InitDecryptor_Internal();
                    m_decryptionMode = true;
                    m_encDecInitialized = true;
                }
            }

            CryptoBuffer OpenSSLCipher::EncryptBuffer(const CryptoBuffer& unEncryptedData)
            {
                if (m_failure)
                {
                    AWS_LOGSTREAM_FATAL(OPENSSL_LOG_TAG, "Cipher not properly initialized for encryption. Aborting");
                    return CryptoBuffer();
                }

                CheckInitEncryptor();
                int lengthWritten = static_cast<int>(unEncryptedData.GetLength() + (GetBlockSizeBytes() - 1));
                CryptoBuffer encryptedText(static_cast<size_t>( lengthWritten + (GetBlockSizeBytes() - 1)));

                if (!EVP_EncryptUpdate(&m_ctx, encryptedText.GetUnderlyingData(), &lengthWritten,
                                       unEncryptedData.GetUnderlyingData(),
                                       static_cast<int>(unEncryptedData.GetLength())))
                {
                    m_failure = true;
                    LogErrors();
                    return CryptoBuffer();
                }

                if (static_cast<size_t>(lengthWritten) < encryptedText.GetLength())
                {
                    return CryptoBuffer(encryptedText.GetUnderlyingData(), static_cast<size_t>(lengthWritten));
                }

                return encryptedText;
            }

            CryptoBuffer OpenSSLCipher::FinalizeEncryption()
            {
                if (m_failure)
                {
                    AWS_LOGSTREAM_FATAL(OPENSSL_LOG_TAG,
                                        "Cipher not properly initialized for encryption finalization. Aborting");
                    return CryptoBuffer();
                }

                CryptoBuffer finalBlock(GetBlockSizeBytes());
                int writtenSize = 0;
                if (!EVP_EncryptFinal_ex(&m_ctx, finalBlock.GetUnderlyingData(), &writtenSize))
                {
                    m_failure = true;
                    LogErrors();
                    return CryptoBuffer();
                }
                return CryptoBuffer(finalBlock.GetUnderlyingData(), static_cast<size_t>(writtenSize));
            }

            CryptoBuffer OpenSSLCipher::DecryptBuffer(const CryptoBuffer& encryptedData)
            {
                if (m_failure)
                {
                    AWS_LOGSTREAM_FATAL(OPENSSL_LOG_TAG, "Cipher not properly initialized for decryption. Aborting");
                    return CryptoBuffer();
                }

                CheckInitDecryptor();
                int lengthWritten = static_cast<int>(encryptedData.GetLength() + (GetBlockSizeBytes() - 1));
                CryptoBuffer decryptedText(static_cast<size_t>(lengthWritten));

                if (!EVP_DecryptUpdate(&m_ctx, decryptedText.GetUnderlyingData(), &lengthWritten,
                                       encryptedData.GetUnderlyingData(),
                                       static_cast<int>(encryptedData.GetLength())))
                {
                    m_failure = true;
                    LogErrors();
                    return CryptoBuffer();
                }

                if (static_cast<size_t>(lengthWritten) < decryptedText.GetLength())
                {
                    return CryptoBuffer(decryptedText.GetUnderlyingData(), static_cast<size_t>(lengthWritten));
                }

                return decryptedText;
            }

            CryptoBuffer OpenSSLCipher::FinalizeDecryption()
            {
                if (m_failure)
                {
                    AWS_LOGSTREAM_FATAL(OPENSSL_LOG_TAG,
                                        "Cipher not properly initialized for decryption finalization. Aborting");
                    return CryptoBuffer();
                }

                CryptoBuffer finalBlock(GetBlockSizeBytes());
                int writtenSize = static_cast<int>(finalBlock.GetLength());
                if (!EVP_DecryptFinal_ex(&m_ctx, finalBlock.GetUnderlyingData(), &writtenSize))
                {
                    m_failure = true;
                    LogErrors();
                    return CryptoBuffer();
                }
                return CryptoBuffer(finalBlock.GetUnderlyingData(), static_cast<size_t>(writtenSize));
            }

            void OpenSSLCipher::Reset()
            {
                Cleanup();
                Init();
            }

            void OpenSSLCipher::Cleanup()
            {
                m_failure = false;
                m_encDecInitialized = false;
                m_encryptionMode = false;
                m_decryptionMode = false;

                if(m_ctx.cipher || m_ctx.cipher_data || m_ctx.engine)
                {
                    EVP_CIPHER_CTX_cleanup(&m_ctx);
                }

                m_ctx.cipher = nullptr;
                m_ctx.cipher_data = nullptr;
                m_ctx.engine = nullptr;
            }

            size_t AES_CBC_Cipher_OpenSSL::BlockSizeBytes = 16;
            size_t AES_CBC_Cipher_OpenSSL::KeyLengthBits = 256;
            static const char* CBC_LOG_TAG = "AES_CBC_Cipher_OpenSSL";

            AES_CBC_Cipher_OpenSSL::AES_CBC_Cipher_OpenSSL(const CryptoBuffer& key) : OpenSSLCipher(key, BlockSizeBytes)
            { }

            AES_CBC_Cipher_OpenSSL::AES_CBC_Cipher_OpenSSL(CryptoBuffer&& key, CryptoBuffer&& initializationVector) :
                    OpenSSLCipher(std::move(key), std::move(initializationVector))
            { }

            AES_CBC_Cipher_OpenSSL::AES_CBC_Cipher_OpenSSL(const CryptoBuffer& key,
                                                           const CryptoBuffer& initializationVector) :
                    OpenSSLCipher(key, initializationVector)
            { }

            void AES_CBC_Cipher_OpenSSL::InitEncryptor_Internal()
            {
                if (!EVP_EncryptInit_ex(&m_ctx, EVP_aes_256_cbc(), nullptr, m_key.GetUnderlyingData(),
                                        m_initializationVector.GetUnderlyingData()))
                {
                    m_failure = true;
                    LogErrors(CBC_LOG_TAG);
                }
            }

            void AES_CBC_Cipher_OpenSSL::InitDecryptor_Internal()
            {
                if (!EVP_DecryptInit_ex(&m_ctx, EVP_aes_256_cbc(), nullptr, m_key.GetUnderlyingData(),
                                        m_initializationVector.GetUnderlyingData()))
                {
                    m_failure = true;
                    LogErrors(CBC_LOG_TAG);
                }
            }

            size_t AES_CBC_Cipher_OpenSSL::GetBlockSizeBytes() const
            {
                return BlockSizeBytes;
            }

            size_t AES_CBC_Cipher_OpenSSL::GetKeyLengthBits() const
            {
                return KeyLengthBits;
            }

            size_t AES_CTR_Cipher_OpenSSL::BlockSizeBytes = 16;
            size_t AES_CTR_Cipher_OpenSSL::KeyLengthBits = 256;
            static const char* CTR_LOG_TAG = "AES_CTR_Cipher_OpenSSL";

            AES_CTR_Cipher_OpenSSL::AES_CTR_Cipher_OpenSSL(const CryptoBuffer& key) : OpenSSLCipher(key, BlockSizeBytes,
                                                                                                    true)
            { }

            AES_CTR_Cipher_OpenSSL::AES_CTR_Cipher_OpenSSL(CryptoBuffer&& key, CryptoBuffer&& initializationVector) :
                    OpenSSLCipher(std::move(key), std::move(initializationVector))
            { }

            AES_CTR_Cipher_OpenSSL::AES_CTR_Cipher_OpenSSL(const CryptoBuffer& key,
                                                           const CryptoBuffer& initializationVector) :
                    OpenSSLCipher(key, initializationVector)
            { }

            void AES_CTR_Cipher_OpenSSL::InitEncryptor_Internal()
            {
                if (!(EVP_EncryptInit_ex(&m_ctx, EVP_aes_256_ctr(), nullptr, m_key.GetUnderlyingData(),
                                         m_initializationVector.GetUnderlyingData())
                        && EVP_CIPHER_CTX_set_padding(&m_ctx, 0)))
                {
                    m_failure = true;
                    LogErrors(CTR_LOG_TAG);
                }
            }

            void AES_CTR_Cipher_OpenSSL::InitDecryptor_Internal()
            {
                if (!(EVP_DecryptInit_ex(&m_ctx, EVP_aes_256_ctr(), nullptr, m_key.GetUnderlyingData(),
                                         m_initializationVector.GetUnderlyingData())
                        && EVP_CIPHER_CTX_set_padding(&m_ctx, 0)))
                {
                    m_failure = true;
                    LogErrors(CTR_LOG_TAG);
                }
            }

            size_t AES_CTR_Cipher_OpenSSL::GetBlockSizeBytes() const
            {
                return BlockSizeBytes;
            }

            size_t AES_CTR_Cipher_OpenSSL::GetKeyLengthBits() const
            {
                return KeyLengthBits;
            }

            size_t AES_GCM_Cipher_OpenSSL::BlockSizeBytes = 16;
            size_t AES_GCM_Cipher_OpenSSL::KeyLengthBits = 256;
            size_t AES_GCM_Cipher_OpenSSL::IVLengthBytes = 12;
            size_t AES_GCM_Cipher_OpenSSL::TagLengthBytes = 16;

            static const char* GCM_LOG_TAG = "AES_GCM_Cipher_OpenSSL";

            AES_GCM_Cipher_OpenSSL::AES_GCM_Cipher_OpenSSL(const CryptoBuffer& key) : OpenSSLCipher(key, IVLengthBytes)
            { }

            AES_GCM_Cipher_OpenSSL::AES_GCM_Cipher_OpenSSL(CryptoBuffer&& key, CryptoBuffer&& initializationVector,
                                                           CryptoBuffer&& tag) :
                    OpenSSLCipher(std::move(key), std::move(initializationVector), std::move(tag))
            { }

            AES_GCM_Cipher_OpenSSL::AES_GCM_Cipher_OpenSSL(const CryptoBuffer& key,
                                                           const CryptoBuffer& initializationVector,
                                                           const CryptoBuffer& tag) :
                    OpenSSLCipher(key, initializationVector, tag)
            { }

            CryptoBuffer AES_GCM_Cipher_OpenSSL::FinalizeEncryption()
            {
                CryptoBuffer&& finalBuffer = OpenSSLCipher::FinalizeEncryption();
                m_tag = CryptoBuffer(TagLengthBytes);
                if (!EVP_CIPHER_CTX_ctrl(&m_ctx, EVP_CTRL_CCM_GET_TAG, static_cast<int>(m_tag.GetLength()),
                                         m_tag.GetUnderlyingData()))
                {
                    m_failure = true;
                    LogErrors(GCM_LOG_TAG);
                    return CryptoBuffer();
                }

                return finalBuffer;
            }

            void AES_GCM_Cipher_OpenSSL::InitEncryptor_Internal()
            {
                if (!(EVP_EncryptInit_ex(&m_ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) &&
                        EVP_EncryptInit_ex(&m_ctx, nullptr, nullptr, m_key.GetUnderlyingData(),
                                           m_initializationVector.GetUnderlyingData()) &&
                        EVP_CIPHER_CTX_set_padding(&m_ctx, 0)))
                {
                    m_failure = true;
                    LogErrors(GCM_LOG_TAG);
                }
            }

            void AES_GCM_Cipher_OpenSSL::InitDecryptor_Internal()
            {
                if (!(EVP_DecryptInit_ex(&m_ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) &&
                        EVP_DecryptInit_ex(&m_ctx, nullptr, nullptr, m_key.GetUnderlyingData(),
                                           m_initializationVector.GetUnderlyingData()) &&
                        EVP_CIPHER_CTX_set_padding(&m_ctx, 0)))
                {
                    m_failure = true;
                    LogErrors(GCM_LOG_TAG);
                    return;
                }

                //tag should always be set in GCM decrypt mode
                assert(m_tag.GetLength() > 0);

                if (m_tag.GetLength() < TagLengthBytes)
                {
                    AWS_LOGSTREAM_ERROR(GCM_LOG_TAG,
                                        "Illegal attempt to decrypt an AES GCM payload without a valid tag set: tag length=" <<
                                                m_tag.GetLength());
                    m_failure = true;
                    return;
                }

                if (!EVP_CIPHER_CTX_ctrl(&m_ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(m_tag.GetLength()),
                                         m_tag.GetUnderlyingData()))
                {
                    m_failure = true;
                    LogErrors(GCM_LOG_TAG);
                }
            }

            size_t AES_GCM_Cipher_OpenSSL::GetBlockSizeBytes() const
            {
                return BlockSizeBytes;
            }

            size_t AES_GCM_Cipher_OpenSSL::GetKeyLengthBits() const
            {
                return KeyLengthBits;
            }

            size_t AES_GCM_Cipher_OpenSSL::GetTagLengthBytes() const
            {
                return TagLengthBytes;
            }

            size_t AES_KeyWrap_Cipher_OpenSSL::KeyLengthBits = 256;
            size_t AES_KeyWrap_Cipher_OpenSSL::BlockSizeBytes = 8;

            static const char* KEY_WRAP_TAG = "AES_KeyWrap_Cipher_OpenSSL";
            static const uint64_t KEY_WRAP_DEFAULT_IV = 0xA6A6A6A6A6A6A6A6;
            static const uint64_t KEY_WRAP_INVARIANT =  0x40; 
            static const uint64_t MSB_CONST = 0x8000000000000000;
            static const uint64_t LSB_CONST = 0x0000000000000001;            

            static uint64_t Msb(uint8_t j, uint64_t buff)
            {
                uint8_t mask(0);
	        for(size_t i = 0; i < j; ++i)
                {
                   mask >>= 1;
                   mask |= MSB_CONST;
                }

                return buff & mask;
            }

            static uint64_t Lsb(uint8_t j, uint64_t buff)
            {
		uint8_t mask(0);
                for(size_t i = 0; i < j; ++i)
                {
                    mask <<= 1;
                    mask |= LSB_CONST;
                }

                return buff & mask;               
            }

            static uint64_t ConvertBufferto64BitInteger(const ByteBuffer& buffer)
            {
                assert(buffer.GetLength() >= sizeof(uint64_t));
                uint64_t value(0);

                for(size_t i = 0; i < sizeof(uint64_t); ++i)
                {
                    value <<= 8;
                    value |= buffer[i];

                }

                return value;
            }

            static Aws::Vector<uint64_t> ConvertBufferTo8ByteSlices(const ByteBuffer& buffer)
            {
                Aws::Vector<uint64_t> values(buffer.GetLength() + (buffer.GetLength() % 8));;

                for(size_t i = 0; i< values.size(); ++i)
                {
                   values[i] = ConvertBufferTo64BitInteger(CryptoBuffer(buffer.GetUnderlyingData() + (8 * i), 8));
                }

                return values;
            }

            AES_KeyWrap_Cipher_OpenSSL::AES_KeyWrap_Cipher_OpenSSL(const CryptoBuffer& key) : OpenSSLCipher(key, 0)
            {
            }

            CryptoBuffer AES_KeyWrap_Cipher_OpenSSL::EncryptBuffer(const CryptoBuffer& plainText)
            {
               CheckInitEncryptor();
               m_workingKeyBuffer = CryptoBuffer({&m_workingKeyBuffer, (CryptoBuffer*)&plainText});
               return CryptoBuffer();
            }

	    CryptoBuffer AES_KeyWrap_Cipher_OpenSSL::FinalizeEncryption()
            {
               CheckInitEncryptor();
               CryptoBuffer integrityCheckRegister(KEY_WRAP_DEFAULT_IV, KEY_WRAP_IV_SIZE);
              
               auto plainTextBlocks = integrityCheckRegister.Slice(BlockSizeBytes);
               size_t n = plainTextBlocks.GetLength();
               //rfc was worded weirdly, I think they meant assign the whole thing
               auto registerBlocks = plainTextBlocks;

               for(size_t j = 0; j < 5; ++j)
               {
                   //rfc was worded weirdly, I think they meant i = 0;
                   for(size_t i = 0; i < n; ++i)
                   {
                       auto B = OpenSSLCipher::Encrypt(CryptoBuffer({&integrityCheckRegister, &registerBlocks[i]}));
                       size_t t = (n * j) + i;
                       
                   }
               }
            }            

            void AES_KeyWrap_Cipher_OpenSSL::InitEncryptor_Internal()
            {
                if (!EVP_EncryptInit_ex(&m_ctx, EVP_aes_256_ecb(), nullptr, m_key.GetUnderlyingData(), nullptr))
                {
                    m_failure = true;
                    LogErrors(KEY_WRAP_TAG);
                }
            }

            void AES_KeyWrap_Cipher_OpenSSL::InitDecryptor_Internal()
            {
                if (!EVP_DecryptInit_ex(&m_ctx, EVP_aes_256_ecb(), nullptr, m_key.GetUnderlyingData(), nullptr))
                {
                    m_failure = true;
                    LogErrors(KEY_WRAP_TAG);
                    return;
                }
            }
        }
    }
}
