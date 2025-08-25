#include <cpuid.h>
#include <sched.h>
#include <pthread.h>
#include <algorithm>
#include <sys/mman.h>

#include "job.h"

bool AESSupport()
{
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return (ecx & bit_AES) != 0;
    return false;
};

bool LargePagesSupport()
{
    void* ptr = mmap(nullptr, 2 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (ptr == MAP_FAILED)
        return false;

    munmap(ptr, 2 * 1024 * 1024);
    return true;
};

randomx::job::job() 
{
    m_machine = std::make_shared<randomx_machine>();
};

randomx::job::~job() 
{

};

uint32_t randomx::job::setBlob(const std::string &blob)
{
    m_size = blob.size() / 2;
    if (sodium_hex2bin(m_blob, sizeof(m_blob), blob.c_str(), blob.size(), nullptr, nullptr, nullptr) != 0)
        return 0;

    uint32_t num_transactions = 0;
    const size_t expected_tx_offset = 75;
    if ((m_size > expected_tx_offset) && (m_size <= expected_tx_offset + 4))
    {
        for (size_t i = expected_tx_offset, k = 0; i < m_size; ++i, k += 7)
        {
            const uint8_t b = m_blob[i];
            num_transactions |= static_cast<uint32_t>(b & 0x7F) << k;
            if ((b & 0x80) == 0)
                break;
        };
    };

    m_nicehash = readUnaligned(nonce()) != 0;

    if (m_machine->machine.size() > 0)
        for (std::shared_ptr<t_machine> machine : m_machine->machine)
            std::memcpy(machine->blob, m_blob, sizeof(m_blob));
        
    return num_transactions;
};

uint64_t randomx::job::setTarget(const std::string &target)
{
    uint8_t raw[4];
    if (sodium_hex2bin(raw, sizeof(raw), target.c_str(), target.size(), nullptr, nullptr, nullptr) != 0)
        return 0;

    m_target = 0xFFFFFFFFFFFFFFFFULL / (0xFFFFFFFFULL / uint64_t(*reinterpret_cast<const uint32_t *>(raw)));
    m_diff = static_cast<uint64_t>(0xFFFFFFFFFFFFFFFFULL / m_target);
    return m_diff;
};

bool randomx::job::alloc(const std::string &mode)
{
    if (m_dataset)
    {
        randomx_release_dataset(m_dataset);
        m_dataset = nullptr;
    };

    if (m_cache)
    {
        randomx_release_cache(m_cache);
        m_cache = nullptr;
    };

    randomx_flags flags = RANDOMX_FLAG_JIT | RANDOMX_FLAG_ARGON2;
    if (AESSupport())
        flags |= RANDOMX_FLAG_HARD_AES;

    if (LargePagesSupport())
        flags |= RANDOMX_FLAG_LARGE_PAGES;

    m_cache = randomx_alloc_cache(flags);
    if (!m_cache)
        return false;

    if (mode == "FAST")
    {
        m_dataset = randomx_alloc_dataset(RANDOMX_FLAG_FULL_MEM);
        if (!m_dataset)
            return false;
    };

    return true;
};

bool randomx::job::init(const std::string &mode, size_t threads, const std::string &seed_hash)
{
    if (!m_cache || (!m_dataset && mode == "FAST"))
        return false;

    if (sodium_hex2bin(m_seed, sizeof(m_seed), seed_hash.c_str(), seed_hash.size(), nullptr, nullptr, nullptr) != 0)
        return false;

    randomx_init_cache(m_cache, m_seed, sizeof(m_seed));
    if (mode == "FAST")
    {
        const uint64_t dataset_count = randomx_dataset_item_count();
        if (threads > 1)
        {
            std::vector<std::thread> m_threads;
            m_threads.reserve(threads);

            for (uint64_t i = 0; i < threads; ++i)
            {
                const uint32_t start = (dataset_count * i) / threads;
                const uint32_t end = (dataset_count * (i + 1)) / threads;

                m_threads.emplace_back([this, start, size = end - start]()
                    {
                        if (size % 5)
                        {
                            randomx_init_dataset(m_dataset, m_cache, start, size - (size % 5));
                            randomx_init_dataset(m_dataset, m_cache, start + size - 5, 5);
                        }
                        else
                            randomx_init_dataset(m_dataset, m_cache, start, size);
                    });
            };

            for (auto &t : m_threads)
            {
                if (t.joinable())
                    t.join();
            };
        }
        else
            randomx_init_dataset(m_dataset, m_cache, 0, dataset_count);

        randomx_release_cache(m_cache);
        m_cache = nullptr;
    };

    return true;
};

void randomx::job::pause()
{
    m_machine->paused = true;
};

void randomx::job::start()
{
    m_machine->paused = false;
};

int randomx::job::hashrate()
{
    int hashes = m_hashes.load(std::memory_order_relaxed) - m_last_hashes;
    int duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - m_last_time).count();

    m_last_hashes += hashes;
    m_last_time = std::chrono::system_clock::now();
    return hashes / duration;
};

void randomx::job::start(const std::string &mode, size_t threads)
{
    if (m_machine->machine.size() >= 1)
        return;

    Napi::ThreadSafeFunction tsfn = Napi::ThreadSafeFunction::New(jsSubmit->Env(), Napi::Function::New(jsSubmit->Env(), [](const Napi::CallbackInfo &) {}), "jsSubmit", 0, 1);
    for (size_t i = 0; i < threads; i++)
    {
        std::shared_ptr<t_machine> machine = std::make_shared<t_machine>();
        randomx_flags flags = RANDOMX_FLAG_JIT;
        if (AESSupport())
            flags |= RANDOMX_FLAG_HARD_AES;
    
        if (mode == "FAST")
            flags |= RANDOMX_FLAG_FULL_MEM;

        machine->vm = randomx_create_vm(flags, m_cache, m_dataset);
        if (!machine->vm)
            continue; 

        std::memcpy(machine->blob, m_blob, kMaxBlobSize);

        machine->nonce = i;
        machine->m_thread = std::thread([this, i, threads, machine, tsfn]()
            {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);

                CPU_SET(i, &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

                while (!m_machine->closed)
                {
                    if (m_machine->paused)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    };

                    calculate_hash(machine->vm, machine->blob, m_size, m_nicehash ? machine->nonce & 0xFFFFFF : machine->nonce, tsfn);
                    if (m_nicehash)
                        machine->nonce = (machine->nonce + threads) & 0xFFFFFF;
                    else
                        machine->nonce += threads;

                    m_hashes.fetch_add(1, std::memory_order_relaxed);
                };
            });
        m_machine->machine.emplace_back(machine);
    };

    m_hashes.store(0, std::memory_order_relaxed);
    m_last_hashes = 0;
};

void randomx::job::calculate_hash(randomx_vm *vm, uint8_t blob[kMaxBlobSize], size_t size, uint32_t n, Napi::ThreadSafeFunction tsfn)
{
    uint8_t result[kMaxSeedSize];
    std::memcpy(nonce(blob), &n, m_nicehash ? 3 : 4);

    randomx_calculate_hash(vm, blob, size, result);
    if (*reinterpret_cast<uint64_t *>(result + 24) < m_target)
    {
        uint32_t m_nonce;
        std::memcpy(&m_nonce, nonce(blob), sizeof(m_nonce));

        char nonce[9];
        if (sodium_bin2hex(nonce, sizeof(nonce), reinterpret_cast<unsigned char *>(&m_nonce), sizeof(m_nonce)) != 0)
            return;

        char result_hex[(kMaxSeedSize * 2) + 1];
        if(sodium_bin2hex(result_hex, sizeof(result_hex), reinterpret_cast<unsigned char*>(&result), sizeof(result)) != 0)
            return;

        tsfn.NonBlockingCall([this, nonce, result_hex](Napi::Env env, Napi::Function)
            {
                jsSubmit->Call({ ToString(env, job_id), ToString(env, nonce), ToString(env, result_hex), ToNumber(env, m_diff) });
            });
    };
};