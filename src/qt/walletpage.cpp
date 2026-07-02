// Copyright (c) 2011-present The TKNC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/walletpage.h>
#include <qt/guiutil.h>
#include <qt/tkncunits.h>  // Note: File renamed internally to TKNCUnits
#include <qt/walletmodel.h>
#include <qt/platformstyle.h>
#include <qt/optionsmodel.h>
#include <qt/receivecoinsdialog.h>
#include <qt/sendcoinsdialog.h>
#include <qt/transactionview.h>
#include <qt/apikeypage.h>  // API Key management page

#include <interfaces/wallet.h>
#include <wallet/wallet.h>

#include <QHeaderView>
#include <QMessageBox>
#include <QDateTime>
#include <QTimer>
#include <QDebug>

WalletPage::WalletPage(const PlatformStyle* _platformStyle, QWidget* parent) 
    : QWidget(parent)
    , platformStyle(_platformStyle)
{
    SetupUI();
    ConnectSignals();
}

void WalletPage::SetupUI()
{
    mainLayout = new QVBoxLayout(this);
    
    // Balance display group
    balanceGroup = new QGroupBox(tr("Balance"), this);
    QVBoxLayout* balanceLayout = new QVBoxLayout(balanceGroup);
    
    totalBalanceLabel = new QLabel(tr("Total Balance: 0 TKNC"), this);
    totalBalanceLabel->setStyleSheet("font-size: 24px; font-weight: bold;");
    balanceLayout->addWidget(totalBalanceLabel);

    availableBalanceLabel = new QLabel(tr("Available: 0 TKNC"), this);
    availableBalanceLabel->setStyleSheet("font-size: 16px;");
    balanceLayout->addWidget(availableBalanceLabel);

    frozenBalanceLabel = new QLabel(tr("Frozen: 0 TKNC (incl. unconfirmed and mining rewards)"), this);
    frozenBalanceLabel->setStyleSheet("font-size: 14px; color: #666;");
    balanceLayout->addWidget(frozenBalanceLabel);
    
    mainLayout->addWidget(balanceGroup);
    
    // Action buttons group
    actionsGroup = new QGroupBox(tr("Actions"), this);
    QHBoxLayout* actionsLayout = new QHBoxLayout(actionsGroup);
    
    receiveButton = new QPushButton(tr("Receive"), this);
    sendButton = new QPushButton(tr("Send"), this);
    buyAPIKeyButton = new QPushButton(tr("Buy API Key"), this);
    historyButton = new QPushButton(tr("History"), this);
    
    actionsLayout->addWidget(receiveButton);
    actionsLayout->addWidget(sendButton);
    actionsLayout->addWidget(buyAPIKeyButton);
    actionsLayout->addWidget(historyButton);
    
    mainLayout->addWidget(actionsGroup);
    
    // Transaction history table
    historyGroup = new QGroupBox(tr("Recent Transactions"), this);
    QVBoxLayout* historyLayout = new QVBoxLayout(historyGroup);
    
    transactionTable = new QTableWidget(this);
    transactionTable->setColumnCount(4);
    transactionTable->setHorizontalHeaderLabels({tr("Time"), tr("Type"), tr("Amount"), tr("Status")});
    transactionTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    transactionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    transactionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    transactionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    
    historyLayout->addWidget(transactionTable);
    mainLayout->addWidget(historyGroup);

    // Initial state: show "Waiting for wallet to load"
    if (!walletModel) {
        totalBalanceLabel->setText(tr("Total Balance: Waiting for wallet..."));
        availableBalanceLabel->setText(tr("Available: Waiting for wallet..."));
        frozenBalanceLabel->setText(tr("Frozen: Waiting for wallet..."));
        
        // Disable action buttons until wallet is loaded
        receiveButton->setEnabled(false);
        sendButton->setEnabled(false);
        buyAPIKeyButton->setEnabled(false);
        historyButton->setEnabled(false);
    }
}

void WalletPage::ConnectSignals()
{
    connect(receiveButton, &QPushButton::clicked, this, &WalletPage::ShowReceiveDialog);
    connect(sendButton, &QPushButton::clicked, this, &WalletPage::SendTransaction);
    connect(buyAPIKeyButton, &QPushButton::clicked, this, &WalletPage::BuyAPIKey);
    connect(historyButton, &QPushButton::clicked, this, &WalletPage::ShowTransactionHistory);
}

void WalletPage::setWalletModel(WalletModel* model)
{
    walletModel = model;
    
    if (!walletModel) {
        qWarning() << "WalletPage: WalletModel is null";
        return;
    }

    // Enable action buttons
    receiveButton->setEnabled(true);
    sendButton->setEnabled(true);
    buyAPIKeyButton->setEnabled(true);
    historyButton->setEnabled(true);

    // Connect WalletModel signals to this page's slots
    connect(walletModel, &WalletModel::balanceChanged, 
            this, &WalletPage::setBalance);
    
    // Initialize display with current balance
    const auto& balances = walletModel->getCachedBalance();
    setBalance(balances);

    qDebug() << "WalletPage: WalletModel connected, initial balance:"
             << balances.balance << "tokens ("
             << TKNCUnits::format(TKNCUnit::TKNC, balances.balance) << "TKNC)";
}

void WalletPage::setClientModel(ClientModel* clientModel)
{
    if (!clientModel) return;

    // ClientModel-related features can be added here
    // e.g., connect node status, block height, etc.
}

void WalletPage::setBalance(const interfaces::WalletBalances& balances)
{
    if (!walletModel) {
        qWarning() << "WalletPage: setBalance called but WalletModel is null";
        return;
    }

    // Get current display unit (default: TKNC)
    TKNCUnit unit = walletModel->getOptionsModel() ?
                       static_cast<TKNCUnit>(walletModel->getOptionsModel()->getDisplayUnit()) :
                       TKNCUnit::TKNC;

    CAmount totalBalance = balances.balance + balances.unconfirmed_balance + balances.immature_balance;
    CAmount availableBalance = balances.balance;
    CAmount frozenBalance = balances.unconfirmed_balance + balances.immature_balance;

    // Format and display balance
    QString totalStr = TKNCUnits::formatWithUnit(unit, totalBalance, false, TKNCUnits::SeparatorStyle::ALWAYS);
    QString availableStr = TKNCUnits::formatWithUnit(unit, availableBalance, false, TKNCUnits::SeparatorStyle::ALWAYS);
    QString frozenStr = TKNCUnits::formatWithUnit(unit, frozenBalance, false, TKNCUnits::SeparatorStyle::ALWAYS);

    totalBalanceLabel->setText(tr("Total Balance: %1").arg(totalStr));
    availableBalanceLabel->setText(tr("Available: %1").arg(availableStr));
    
    if (frozenBalance > 0) {
        frozenBalanceLabel->setText(tr("Frozen: %1 (incl. unconfirmed and mining rewards)").arg(frozenStr));
    } else {
        frozenBalanceLabel->setText(tr("No frozen amount"));
    }

    qDebug() << "WalletPage: Balance updated - Total:" << totalStr 
             << "Available:" << availableStr << "Frozen:" << frozenStr;
}

void WalletPage::ShowReceiveDialog()
{
    if (!walletModel) {
        QMessageBox::warning(this, tr("Error"), tr("Wallet not loaded"));
        return;
    }

    // Open TKNC native ReceiveCoinsDialog
    ReceiveCoinsDialog* dialog = new ReceiveCoinsDialog(platformStyle, walletModel, this);
    
    // Set dialog attribute as modal window
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->exec();
}

void WalletPage::SendTransaction()
{
    if (!walletModel) {
        QMessageBox::warning(this, tr("Error"), tr("Wallet not loaded"));
        return;
    }

    // Check if wallet is encrypted and locked
    if (walletModel->getEncryptionStatus() == WalletModel::Locked) {
        QMessageBox::information(this, tr("Wallet Locked"), 
            tr("Please unlock the wallet before sending transactions.\n"
               "Menu: File → Unlock Wallet"));
        return;
    }

    // Open TKNC native SendCoinsDialog
    SendCoinsDialog* dialog = new SendCoinsDialog(platformStyle, walletModel, this);
    
    // Set dialog attribute
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    
    // Refresh balance if transaction was sent successfully
    int result = dialog->exec();
    if (result == QDialog::Accepted && walletModel) {
        const auto& balances = walletModel->getCachedBalance();
        setBalance(balances);
    }
}

void WalletPage::BuyAPIKey()
{
    if (!walletModel) {
        QMessageBox::warning(this, tr("Error"), tr("Wallet not loaded"));
        return;
    }

    // Open APIKeyPage dialog for API Key purchase/creation
    // Use modal dialog style consistent with TKNC native dialogs
    APIKeyPage* apiKeyDialog = new APIKeyPage(platformStyle, this);
    
    // Pass WalletModel and ClientModel (if available)
    apiKeyDialog->setWalletModel(walletModel);
    if (clientModel) {
        apiKeyDialog->setClientModel(clientModel);
    }
    
    // Set dialog attributes
    apiKeyDialog->setWindowTitle(tr("TOKENcoin - API Key Management"));
    apiKeyDialog->setAttribute(Qt::WA_DeleteOnClose);
    apiKeyDialog->setMinimumSize(800, 600);
    
    // Show dialog
    qDebug() << "WalletPage: Opening API Key management dialog";
    apiKeyDialog->exec();
}

void WalletPage::ShowTransactionHistory()
{
    if (!walletModel) {
        QMessageBox::warning(this, tr("Error"), tr("Wallet not loaded"));
        return;
    }

    // Open TKNC native TransactionView window for full transaction history
    // Use independent window mode consistent with TKNC Qt standard behavior
    TransactionView* transactionView = new TransactionView(platformStyle, walletModel, this);
    
    // Set window attributes
    transactionView->setWindowTitle(tr("TokenCoin - Transaction History"));
    transactionView->setAttribute(Qt::WA_DeleteOnClose);
    transactionView->setMinimumSize(900, 600);
    
    // Show as independent window (non-modal)
    qDebug() << "WalletPage: Opening transaction history window";
    transactionView->show();
}

void WalletPage::AddTransactionRow(const QString& time, const QString& type, 
                                   const QString& amount, const QString& status)
{
    int row = transactionTable->rowCount();
    transactionTable->insertRow(row);
    transactionTable->setItem(row, 0, new QTableWidgetItem(time));
    transactionTable->setItem(row, 1, new QTableWidgetItem(type));
    transactionTable->setItem(row, 2, new QTableWidgetItem(amount));
    transactionTable->setItem(row, 3, new QTableWidgetItem(status));
}
