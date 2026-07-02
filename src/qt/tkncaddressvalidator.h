// Copyright (c) 2011-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_QT_TKNCADDRESSVALIDATOR_H
#define TKN_QT_TKNCADDRESSVALIDATOR_H

#include <QValidator>

/** Base58 entry widget validator, checks for valid characters and
 * removes some whitespace.
 */
class TKNCAddressEntryValidator : public QValidator
{
    Q_OBJECT

public:
    explicit TKNCAddressEntryValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

/** TKNC address widget validator, checks for a valid TKNC address.
 */
class TKNCAddressCheckValidator : public QValidator
{
    Q_OBJECT

public:
    explicit TKNCAddressCheckValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

#endif // TKN_QT_TKNCADDRESSVALIDATOR_H
