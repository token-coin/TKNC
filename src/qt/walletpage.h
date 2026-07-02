#ifndef TKN_QT_WALLETPAGE_H
#define TKN_QT_WALLETPAGE_H

#include <tknc-build-config.h>
#include <QObject>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QString>
#include <interfaces/wallet.h>

class WalletModel;
class PlatformStyle;
class TransactionTableModel;

class WalletPage : public QWidget {
    Q_OBJECT

private:
    QLabel* totalBalanceLabel;
    QLabel* availableBalanceLabel;
    QLabel* frozenBalanceLabel;
    
    QPushButton* receiveButton;
    QPushButton* sendButton;
    QPushButton* buyAPIKeyButton;
    QPushButton* historyButton;
    
    QTableWidget* transactionTable;
    
    QVBoxLayout* mainLayout;
    QGroupBox* balanceGroup;
    QGroupBox* actionsGroup;
    QGroupBox* historyGroup;

    WalletModel* walletModel{nullptr};
    const PlatformStyle* platformStyle{nullptr};

public:
    explicit WalletPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);

    void setWalletModel(WalletModel* model);
    void setClientModel(class ClientModel* clientModel);

private Q_SLOTS:
    void setBalance(const struct interfaces::WalletBalances& balances);
    
    void ShowReceiveDialog();
    
    void SendTransaction();
    
    void BuyAPIKey();
    
    void ShowTransactionHistory();
    
    void AddTransactionRow(const QString& time, const QString& type, 
                          const QString& amount, const QString& status);

private:
    void SetupUI();
    
    void ConnectSignals();
};

#endif // TKN_QT_WALLETPAGE_H
