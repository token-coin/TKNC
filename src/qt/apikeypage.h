#ifndef TKN_QT_APIKEYPAGE_H
#define TKN_QT_APIKEYPAGE_H

#include <tknc-build-config.h>
#include <QObject>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QTableWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QString>

class WalletModel;
class ClientModel;
class PlatformStyle;

class APIKeyPage : public QWidget {
    Q_OBJECT

private:
    QGroupBox* createGroup;
    QLineEdit* balanceInput;
    QComboBox* modelNameCombo;
    QSpinBox* expiryDaysInput;
    QPushButton* createButton;
    
    QGroupBox* listGroup;
    QTableWidget* keyTable;
    QPushButton* refreshButton;
    QLabel* countLabel;
    
    QGroupBox* actionGroup;
    QPushButton* topupButton;
    QPushButton* revokeButton;
    QPushButton* copyButton;
    
    QLabel* statusLabel;
    
    QVBoxLayout* mainLayout;

    WalletModel* walletModel{nullptr};
    ClientModel* clientModel{nullptr};
    const PlatformStyle* platformStyle{nullptr};

public:
    explicit APIKeyPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);

    void setWalletModel(WalletModel* model);
    void setClientModel(ClientModel* model);

private Q_SLOTS:
    void OnCreateAPIKey();
    
    void OnRefreshList();
    
    void OnTopUpKey();
    
    void OnRevokeKey();
    
    void OnCopyKey();
    
    void OnSelectionChanged();

private:
    void SetupUI();
    
    void ConnectSignals();
    
    void CallRPC(const QString& command, const QStringList& params, std::function<void(const QVariantMap&)> callback);
    
    void SetStatus(const QString& message, bool isError = false);
    
    QString FormatTimestamp(int64_t timestamp);
};

#endif // TKN_QT_APIKEYPAGE_H
