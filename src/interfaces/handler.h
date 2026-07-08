// Copyright (c) 2018-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_INTERFACES_HANDLER_H
#define TKN_INTERFACES_HANDLER_H

#include <functional>
#include <memory>

namespace tkncsignals {
    class connection;
} // namespace tkncsignals

namespace interfaces {

//! Generic interface for managing an event handler or callback function
//! registered with another interface. Has a single disconnect method to cancel
//! the registration and prevent any future notifications.
class Handler
{
public:
    virtual ~Handler() = default;

    //! Disconnect the handler.
    virtual void disconnect() = 0;
};

//! Return handler wrapping a tkncsignals connection.
std::unique_ptr<Handler> MakeSignalHandler(tkncsignals::connection connection);

//! Return handler wrapping a cleanup function.
std::unique_ptr<Handler> MakeCleanupHandler(std::function<void()> cleanup);

} // namespace interfaces

#endif // TKN_INTERFACES_HANDLER_H
