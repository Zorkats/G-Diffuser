// port/net/gdx_net_listing.cpp: see gdx_net_listing.h.
// Copyright (c) Zorkats

#include "gdx_net_listing.h"

#include "gdx_net_codes.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace gdx::net::listing {

using directory::Answer;
using directory::Error;

struct Listing::Slot {
    std::mutex lock;
    bool done = false;
    Answer<uint32_t> answer;
};

std::string DirectoryCode(uint32_t value) {
    std::string text = codes::FormatRoomCode(value);
    text.erase(std::remove(text.begin(), text.end(), '-'), text.end());
    return text;
}

bool CodeValue(const std::string& text, uint32_t& value) {
    return codes::ParseRoomCode(text, value);
}

directory::Endpoint EndpointOf(const NetAddress& address, directory::How how) {
    directory::Endpoint endpoint;
    endpoint.port = address.port;
    endpoint.how = how;
    const std::string text = FormatAddress(address);
    if (address.IsV4()) {
        endpoint.family = directory::Family::V4;
        endpoint.addr = text.substr(0, text.rfind(':'));
    } else {
        endpoint.family = directory::Family::V6;
        const size_t close = text.find(']');
        endpoint.addr = close == std::string::npos ? std::string() : text.substr(1, close - 1);
    }
    return endpoint;
}

bool AddressOf(const directory::Endpoint& endpoint, NetAddress& out) {
    const bool v4 = endpoint.family == directory::Family::V4;
    const std::string text = v4 ? endpoint.addr + ":" + std::to_string(endpoint.port)
                                : "[" + endpoint.addr + "]:" + std::to_string(endpoint.port);
    NetAddress parsed;
    if (endpoint.port == 0 || !ParseAddress(text.c_str(), parsed) || parsed.IsV4() != v4) {
        return false;
    }
    out = parsed;
    return true;
}

bool Dialable(const NetAddress& address, bool allowLoopback) {
    return DialableIp(address.ip, address.port, allowLoopback);
}

Listing::Listing(directory::Client client, Spawn spawn, Random random)
    : client_(std::move(client)), spawn_(std::move(spawn)), random_(std::move(random)) {}

void Listing::Register(const directory::Draft& draft, uint32_t now) {
    draft_ = draft;
    out_ = Call::None;
    slot_.reset(); // a call still out answers into a slot nobody reads
    ambiguous_ = false;
    probing_ = false;
    codeTries_ = 0;
    random_(secret_.data(), secret_.size());
    state_ = State::Working;
    error_ = Error::None;
    NewCode();
    Send(Call::Register, now);
}

void Listing::Takeover(uint32_t code, const directory::Secret& secret, const directory::Draft& draft, uint32_t now) {
    draft_ = draft;
    out_ = Call::None;
    slot_.reset();
    ambiguous_ = false;
    probing_ = false;
    codeTries_ = 0;
    code_ = code;
    secret_ = secret;
    random_(next_.data(), next_.size());
    serial_++;
    state_ = State::Working;
    error_ = Error::None;
    Send(Call::Takeover, now);
}

void Listing::RotateSecret(uint32_t now) {
    if (state_ != State::Listed || out_ == Call::Register || out_ == Call::Takeover) {
        rotate_ = state_ != State::Failed;
        return;
    }
    rotate_ = false;
    out_ = Call::None;
    slot_.reset(); // a heartbeat still out only refreshes the record
    ambiguous_ = false;
    random_(next_.data(), next_.size());
    Send(Call::Takeover, now);
}

void Listing::SetDraft(const directory::Draft& draft) {
    draft_ = draft;
}

void Listing::Update(uint32_t now) {
    // An answer can lead straight to the next call (code_taken to a fresh register, say), and a job that ran at
    // once has its answer ready, so the chain is followed here. The cap only guards against a runaway chain: each
    // step is bounded on its own (kCodeTries, one probe, one re-register).
    for (int step = 0; step < 16; step++) {
        if (out_ != Call::None && slot_) {
            bool done = false;
            Answer<uint32_t> answer;
            {
                std::lock_guard<std::mutex> guard(slot_->lock);
                done = slot_->done;
                answer = slot_->answer;
            }
            if (!done) {
                return;
            }
            const Call call = out_;
            out_ = Call::None;
            slot_.reset();
            Answered(call, answer, now);
            continue;
        }
        if (rotate_ && out_ == Call::None && state_ == State::Listed) {
            RotateSecret(now);
            continue;
        }
        if (due_ != Call::None && static_cast<int32_t>(now - dueAt_) >= 0) {
            const Call call = due_;
            due_ = Call::None;
            Send(call, now);
            continue;
        }
        return;
    }
}

void Listing::Delist() {
    if ((state_ != State::Listed && state_ != State::Working) || code_ == 0xFFFFFFFFu) {
        return;
    }
    directory::DelistRequest request;
    request.code = DirectoryCode(code_);
    request.secret = secret_;
    const directory::Client client = client_;
    spawn_([client, request]() { (void)client.Delist(request); });
    out_ = Call::None;
    slot_.reset();
    due_ = Call::None;
    rotate_ = false;
    state_ = State::Idle;
}

void Listing::NewCode() {
    if (codeTries_ >= kCodeTries) {
        Fail(Error::CodeTaken);
        return;
    }
    codeTries_++;
    uint8_t bytes[4] = {};
    random_(bytes, sizeof(bytes));
    code_ = codes::RoomCodeFromRandom(bytes);
    serial_++;
}

void Listing::Fail(Error error) {
    state_ = State::Failed;
    error_ = error;
    due_ = Call::None;
}

void Listing::Later(uint32_t now, uint32_t ms, Call call) {
    due_ = call;
    dueAt_ = now + ms;
}

void Listing::Send(Call call, uint32_t now) {
    (void)now;
    if (state_ == State::Failed) {
        return;
    }
    const std::string code = DirectoryCode(code_);
    auto slot = std::make_shared<Slot>();
    std::function<Answer<uint32_t>()> run;
    const directory::Client client = client_;
    if (call == Call::Register) {
        directory::RegisterRequest request;
        request.code = code;
        request.takeoverHash = directory::TakeoverHashOf(secret_);
        request.draft = draft_;
        run = [client, request]() { return client.Register(request); };
    } else if (call == Call::Heartbeat) {
        directory::HeartbeatRequest request;
        request.code = code;
        request.secret = secret_;
        request.draft = draft_;
        run = [client, request]() { return client.Heartbeat(request); };
    } else {
        directory::TakeoverRequest request;
        request.code = code;
        request.secret = secret_;
        request.nextHash = directory::TakeoverHashOf(next_);
        request.draft = draft_;
        run = [client, request]() { return client.Takeover(request); };
    }
    out_ = call;
    slot_ = slot;
    spawn_([slot, run]() {
        const Answer<uint32_t> answer = run();
        std::lock_guard<std::mutex> guard(slot->lock);
        slot->answer = answer;
        slot->done = true;
    });
}

void Listing::Answered(Call call, const Answer<uint32_t>& answer, uint32_t now) {
    const Error error = answer.error;
    const uint32_t wait = (std::max)(kRetryMs, answer.retryAfterSeconds * 1000u);
    if (error == Error::None) {
        if (call == Call::Takeover) {
            secret_ = next_;
            serial_++;
        }
        ambiguous_ = false;
        probing_ = false;
        state_ = State::Listed;
        error_ = Error::None;
        Later(now, kHeartbeatMs, Call::Heartbeat);
        return;
    }
    if (error == Error::RateLimited) {
        Later(now, wait, call);
        return;
    }
    if (error == Error::Unreachable || error == Error::Internal) {
        // A heartbeat only refreshes the record, so retrying it is safe; a register or a takeover may have been
        // applied, which the retry then has to find out.
        ambiguous_ = ambiguous_ || call != Call::Heartbeat;
        error_ = error;
        Later(now, kRetryMs, call);
        return;
    }
    switch (call) {
    case Call::Register:
        if (error == Error::CodeTaken && ambiguous_) {
            // Maybe our own record from the write that got no answer: ask under our secret.
            ambiguous_ = false;
            probing_ = true;
            Send(Call::Heartbeat, now);
            return;
        }
        if (error == Error::CodeTaken) {
            NewCode();
            Send(Call::Register, now);
            return;
        }
        break;
    case Call::Heartbeat:
        if (error == Error::NotFound) {
            // The record ran out (the directory was unreachable for its whole life): list the same code again.
            probing_ = false;
            Send(Call::Register, now);
            return;
        }
        if (error == Error::BadSecret && probing_) {
            probing_ = false;
            NewCode();
            Send(Call::Register, now);
            return;
        }
        break;
    case Call::Takeover:
        if (error == Error::NotFound) {
            // The old record ran out before the successor got to it: list the code afresh under the new secret.
            secret_ = next_;
            serial_++;
            ambiguous_ = false;
            Send(Call::Register, now);
            return;
        }
        if (error == Error::BadSecret && ambiguous_) {
            // The takeover that got no answer was applied, and the old secret died with it.
            secret_ = next_;
            serial_++;
            ambiguous_ = false;
            Send(Call::Heartbeat, now);
            return;
        }
        break;
    case Call::None:
        break;
    }
    Fail(error);
}

} // namespace gdx::net::listing
