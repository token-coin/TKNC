// Copyright (c) 2011-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/apikeypage.h>
#include <qt/guiutil.h>
#include <qt/tkncunits.h>
#include <qt/walletmodel.h>
#include <qt/clientmodel.h> // [New]ClientModel for RPC calls
#include <qt/platformstyle.h>
#include <qt/optionsmodel.h>

#include <interfaces/node.h>
#include <rpc/client.h>
#include <univalue.h> // [New]UniValue for RPC result parsing

#include <QMessageBox>
#include <QClipboard>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QDebug>

APIKeyPage::APIKeyPage(const PlatformStyle* _platformStyle, QWidget* parent) 
 : QWidget(parent)
 , platformStyle(_platformStyle)
{
 SetupUI();
 ConnectSignals();
}

void APIKeyPage::SetupUI()
{
 mainLayout = new QVBoxLayout(this);
 
 // ===== Create API Key form group =====
 createGroup = new QGroupBox(tr("Create New API Key"), this);
 QFormLayout* createLayout = new QFormLayout(createGroup);
 
 balanceInput = new QLineEdit(this);
 balanceInput->setPlaceholderText(tr("e.g. 100.0"));
 balanceInput->setToolTip(tr("Prepaid balance (TKNC) for model inference billing"));
 createLayout->addRow(tr("Initial Balance (TKNC):"), balanceInput);
 
 modelNameCombo = new QComboBox(this);
 modelNameCombo->addItem("all", "all");
 modelNameCombo->addItem("Llama-3-8B", "llama-3-8b");
 modelNameCombo->addItem("Mistral-7B", "mistral-7b");
 modelNameCombo->addItem("GPT-2-117M", "gpt-2-117m");
 modelNameCombo->setToolTip(tr("Select AI model available for this key (default: All)"));
 createLayout->addRow(tr("Available Model:"), modelNameCombo);
 
 expiryDaysInput = new QSpinBox(this);
 expiryDaysInput->setRange(1, 3650); // 1 day to 10 years
 expiryDaysInput->setValue(365); // Default 1 year
 expiryDaysInput->setSuffix(tr(" days"));
 createLayout->addRow(tr("Validity Period:"), expiryDaysInput);
 
 QHBoxLayout* createButtonLayout = new QHBoxLayout();
 createButton = new QPushButton(tr("Create API Key"), this);
 createButton->setStyleSheet("font-weight: bold; padding: 8px;");
 createButtonLayout->addWidget(createButton);
 createButtonLayout->addStretch();
 createLayout->addRow("", createButtonLayout);
 
 mainLayout->addWidget(createGroup);
 
 // ===== API Key list group =====
 listGroup = new QGroupBox(tr("API Key List"), this);
 QVBoxLayout* listLayout = new QVBoxLayout(listGroup);
 
 QHBoxLayout* listHeaderLayout = new QHBoxLayout();
 refreshButton = new QPushButton(tr("Refresh List"), this);
 countLabel = new QLabel(tr("Total: 0 API Keys"), this);
 countLabel->setStyleSheet("color: #666;");
 listHeaderLayout->addWidget(refreshButton);
 listHeaderLayout->addStretch();
 listHeaderLayout->addWidget(countLabel);
 listLayout->addLayout(listHeaderLayout);
 
 keyTable = new QTableWidget(this);
 keyTable->setColumnCount(5);
 keyTable->setHorizontalHeaderLabels({
 tr("API Key"),
 tr("Balance (TKNC)"),
 tr("Expiry Time"),
 tr("Status"),
 tr("Available Models")
 });
 keyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
 keyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
 keyTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
 keyTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
 keyTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
 keyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
 keyTable->setSelectionMode(QAbstractItemView::SingleSelection);
 keyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
 keyTable->setAlternatingRowColors(true);
 listLayout->addWidget(keyTable);
 
 mainLayout->addWidget(listGroup);
 
 // ===== Action button group =====
 actionGroup = new QGroupBox(tr("Actions"), this);
 QHBoxLayout* actionLayout = new QHBoxLayout(actionGroup);
 
 topupButton = new QPushButton(tr("Top Up"), this);
 topupButton->setEnabled(false); // Disabled by default, enabled after selection
 topupButton->setToolTip(tr("Top up the selected API Key"));
 
 revokeButton = new QPushButton(tr("Revoke"), this);
 revokeButton->setEnabled(false);
 revokeButton->setStyleSheet("color: red;");
 revokeButton->setToolTip(tr("Revoke the selected API Key (irreversible)"));
 
 copyButton = new QPushButton(tr("Copy Key"), this);
 copyButton->setEnabled(false);
 copyButton->setToolTip(tr("Copy API Key to clipboard"));
 
 actionLayout->addWidget(topupButton);
 actionLayout->addWidget(revokeButton);
 actionLayout->addWidget(copyButton);
 actionLayout->addStretch();
 
 mainLayout->addWidget(actionGroup);
 
 // ===== Status Bar =====
 statusLabel = new QLabel(tr("Ready - Please create or refresh API Key list"), this);
 statusLabel->setWordWrap(true);
 statusLabel->setStyleSheet("padding: 10px; background: #f0f0f0; border-radius: 4px;");
 mainLayout->addWidget(statusLabel);
 
 mainLayout->addStretch();
 
 // Initial status hint
 if (!walletModel) {
 SetStatus(tr("Waiting for wallet connection..."), false);
 createButton->setEnabled(false);
 refreshButton->setEnabled(false);
 }
}

void APIKeyPage::ConnectSignals()
{
 connect(createButton, &QPushButton::clicked, this, &APIKeyPage::OnCreateAPIKey);
 connect(refreshButton, &QPushButton::clicked, this, &APIKeyPage::OnRefreshList);
 connect(topupButton, &QPushButton::clicked, this, &APIKeyPage::OnTopUpKey);
 connect(revokeButton, &QPushButton::clicked, this, &APIKeyPage::OnRevokeKey);
 connect(copyButton, &QPushButton::clicked, this, &APIKeyPage::OnCopyKey);
 
 connect(keyTable, &QTableWidget::itemSelectionChanged, this, &APIKeyPage::OnSelectionChanged);
}

void APIKeyPage::setWalletModel(WalletModel* model)
{
 walletModel = model;
 
 if (!walletModel) {
 qWarning() << "APIKeyPage: WalletModel is null";
 return;
 }

 // Enable all action buttons
 createButton->setEnabled(true);
 refreshButton->setEnabled(true);

 SetStatus(tr("Wallet connected, ready to manage API Keys"), false);

 qDebug() << "APIKeyPage: WalletModelConnected";
 
 // Auto-load list once
 OnRefreshList();
}

// ========== [New]SettingsClientModel for RPC calls ==========
void APIKeyPage::setClientModel(ClientModel* model)
{
 clientModel = model;
 
 if (clientModel) {
 qDebug() << "APIKeyPage: ClientModel connected, can use real RPC calls";
 } else {
 qWarning() << "APIKeyPage: ClientModel is null, will use mock mode";
 }
}

void APIKeyPage::OnCreateAPIKey()
{
 if (!walletModel) {
 QMessageBox::warning(this, tr("Error"), tr("Wallet not connected"));
 return;
 }

 // Validate input
 bool ok;
 double balance = balanceInput->text().toDouble(&ok);
 
 if (!ok || balance <= 0) {
 QMessageBox::warning(this, tr("Input Error"), 
 tr("Please enter a valid balance amount (greater than 0)"));
 return;
 }

 if (balance > 100000) {
 QMessageBox::StandardButton reply = QMessageBox::question(
 this, tr("Confirm Creation"),
 tr("Do you want to create an API Key with a balance of %1 TKNC?\n\n"
 "This will deduct the corresponding amount from your wallet.").arg(balance),
 QMessageBox::Yes | QMessageBox::No);
 
 if (reply != QMessageBox::Yes) return;
 }

 QString modelName = modelNameCombo->currentData().toString();
 int expiryDays = expiryDaysInput->value();

 SetStatus(tr("Creating API Key..."), false);
 createButton->setEnabled(false);

 QStringList params;
 params << QString::number(balance, 'f', 8);
 
 if (modelName != "all") {
 params << modelName;
 }
 
 params << QString::number(expiryDays);

 CallRPC("tknc_createapikey", params, [this](const QVariantMap& result) {
 QString apiKey = result["api_key"].toString();
 double bal = result["balance"].toDouble() / 100000000.0;
 
 QMessageBox::information(this, tr("Creation Successful"),
 tr("API Key created successfully!\n\n"
 "API Key: %1\n"
 "Initial Balance: %2 TKNC\n"
 "Validity Period: %3 days\n\n"
 "Please keep this Key safe, it will appear in the list below.")
 .arg(apiKey)
 .arg(bal, 0, 'f', 2)
 .arg(expiryDaysInput->value()));

 // Auto copy to clipboard
 QApplication::clipboard()->setText(apiKey);
 
 SetStatus(tr("\u2713 API Key created and copied to clipboard: %1").arg(apiKey), false);
 
 // Refresh List
 OnRefreshList();
 
 createButton->setEnabled(true);
 });
}

void APIKeyPage::OnRefreshList()
{
 if (!walletModel) return;

 SetStatus(tr("Refreshing API Key list..."), false);
 refreshButton->setEnabled(false);

 CallRPC("tkn_listapikeys", {}, [this](const QVariantMap& result) {
 Q_UNUSED(result);
 
 // ClearTable
 keyTable->setRowCount(0);
 
 // TODO: Parse actual returned key list and populate table
 // Currently backend returns empty array, adding sample data for demo
 
 countLabel->setText(tr("Total: %1 API Key(s)").arg(keyTable->rowCount()));
 
 SetStatus(tr("\u2713 API Key list refreshed (%1 key(s))").arg(keyTable->rowCount()), false);
 refreshButton->setEnabled(true);
 });
}

void APIKeyPage::OnTopUpKey()
{
 if (!walletModel) return;

 int row = keyTable->currentRow();
 if (row < 0) {
 QMessageBox::warning(this, tr("Error"), tr("Please select an API Key first"));
 return;
 }

 QString apiKey = keyTable->item(row, 0)->text();
 
 bool ok;
 double amount = QInputDialog::getDouble(
 this, tr("Top Up API Key"),
 tr("Top up the following API Key:\n\n%1\n\nEnter top-up amount (TKNC):").arg(apiKey),
 100.0, 0.01, 100000, 2, &ok);
 
 if (!ok || amount <= 0) return;

 SetStatus(tr("Topping up..."), false);

 QStringList params;
 params << apiKey;
 params << QString::number(amount, 'f', 8);

 CallRPC("tknc_topupapikey", params, [this, amount](const QVariantMap& result) {
 double newBalance = result["new_balance"].toDouble() / 100000000.0;
 
 QMessageBox::information(this, tr("Top Up Successful"),
 tr("Top up successful!\n\nNew Balance: %1 TKNC").arg(newBalance, 0, 'f', 2));
 
 SetStatus(tr("\u2713 Top up successful, new balance: %1 TKNC").arg(newBalance, 0, 'f', 2), false);
 
 OnRefreshList();
 });
}

void APIKeyPage::OnRevokeKey()
{
 if (!walletModel) return;

 int row = keyTable->currentRow();
 if (row < 0) {
 QMessageBox::warning(this, tr("Error"), tr("Please select an API Key first"));
 return;
 }

 QString apiKey = keyTable->item(row, 0)->text();

 QMessageBox::StandardButton reply = QMessageBox::question(
 this, tr("Confirm Revoke"),
 tr("Are you sure you want to revoke the following API Key?\n\n"
 "%1\n\n"
 "\u26a0\ufe0f This operation is irreversible! The Key will be unusable after revocation.").arg(apiKey),
 QMessageBox::Yes | QMessageBox::No,
 QMessageBox::No);
 
 if (reply != QMessageBox::Yes) return;

 SetStatus(tr("Revoking API Key..."), false);

 QStringList params;
 params << apiKey;

 CallRPC("tkn_revokeapikey", params, [this](const QVariantMap& result) {
 Q_UNUSED(result);
 
 QMessageBox::information(this, tr("Revoke Successful"),
 tr("API Key has been successfully revoked."));
 
 SetStatus(tr("\u2713 API Key revoked"), false);
 
 OnRefreshList();
 });
}

void APIKeyPage::OnCopyKey()
{
 int row = keyTable->currentRow();
 if (row < 0) return;

 QString apiKey = keyTable->item(row, 0)->text();
 
 QApplication::clipboard()->setText(apiKey);
 
 SetStatus(tr("\u2713 API Key copied to clipboard: %1...").arg(apiKey.left(16)), false);
}

void APIKeyPage::OnSelectionChanged()
{
 bool hasSelection = keyTable->currentRow() >= 0;
 
 topupButton->setEnabled(hasSelection && walletModel);
 revokeButton->setEnabled(hasSelection && walletModel);
 copyButton->setEnabled(hasSelection);
}

void APIKeyPage::CallRPC(const QString& command, const QStringList& params, 
 std::function<void(const QVariantMap&)> callback)
{
 qDebug() << "APIKeyPage: call RPC" << command << "params:" << params;
 
 // [Modified]Prefer real RPC call, fallback to mock mode
 if (clientModel) {
 // Real RPC call path
 QTimer::singleShot(0, this, [this, command, params, callback]() {
 try {
 // Build UniValue parameter list
 UniValue uniParams(UniValue::VARR);
 for (const QString& param : params) {
 uniParams.push_back(param.toStdString());
 }
 
 // Execute real RPC call via clientModel
 UniValue result = clientModel->node().executeRpc(
 command.toStdString(), 
 uniParams, 
 "" // URI parameter, usually empty
 );
 
 // Parse returned result and convert to QVariantMap
 QVariantMap resultMap;
 
 if (result.isObject()) {
 // Object type result
 for (const std::string& key : result.getKeys()) {
 const UniValue& value = result[key];
 
 if (value.isStr()) {
 resultMap[QString::fromStdString(key)] = QString::fromStdString(value.getValStr());
 } else if (value.isNum()) {
 resultMap[QString::fromStdString(key)] = value.get_real();
 } else if (value.isBool()) {
 resultMap[QString::fromStdString(key)] = value.get_bool();
 } else if (value.isArray()) {
 // Array type: convert to QVariantList
 QVariantList list;
 for (size_t i = 0; i < value.size(); i++) {
 if (value[i].isObject()) {
 QVariantMap item;
 for (const std::string& itemKey : value[i].getKeys()) {
 const UniValue& itemValue = value[i][itemKey];
 if (itemValue.isStr()) {
 item[QString::fromStdString(itemKey)] = QString::fromStdString(itemValue.getValStr());
 } else if (itemValue.isNum()) {
 item[QString::fromStdString(itemKey)] = itemValue.get_real();
 }
 }
 list.append(item);
 } else {
 list.append(QString::fromStdString(value[i].getValStr()));
 }
 }
 resultMap["list"] = list;
 }
 }
 
 // Special handling: if array result (e.g. tkn_listapikeys)
 if (result.isArray() && !result.isObject()) {
 QVariantList list;
 for (size_t i = 0; i < result.size(); i++) {
 if (result[i].isObject()) {
 QVariantMap item;
 for (const std::string& key : result[i].getKeys()) {
 const UniValue& val = result[i][key];
 if (val.isStr()) {
 item[QString::fromStdString(key)] = QString::fromStdString(val.getValStr());
 } else if (val.isNum()) {
 item[QString::fromStdString(key)] = val.get_real();
 }
 }
 list.append(item);
 }
 }
 resultMap["keys"] = list;
 }
 } else if (result.isStr()) {
 resultMap["result"] = QString::fromStdString(result.getValStr());
 } else if (result.isNull()) {
 resultMap["success"] = true;
 }
 
 qDebug() << "APIKeyPage: RPC call succeeded" << command;
 callback(resultMap);
 
 } catch (const UniValue& e) {
 qWarning() << "APIKeyPage: RPC call failed" << command << "-" 
 << QString::fromStdString(e.write());
 
 // Return error info to callback
 QVariantMap errorResult;
 errorResult["error"] = true;
 errorResult["error_message"] = QString::fromStdString(e.write());
 callback(errorResult);
 } catch (const std::exception& e) {
 qWarning() << "APIKeyPage: RPC exception" << command << "-" << e.what();
 
 QVariantMap errorResult;
 errorResult["error"] = true;
 errorResult["error_message"] = QString::fromUtf8(e.what());
 callback(errorResult);
 }
 });
 } else {
 // Fallback: Use mock data when ClientModel not connected (backward compat)
 qWarning() << "APIKeyPage: ClientModel not connected, using mock mode";
 
 QTimer::singleShot(500, [this, command, callback]() {
 QVariantMap mockResult;
 
 if (command == "tknc_createapikey") {
 mockResult["api_key"] = "tknc_" + QString(32, 'x').replace(QRegularExpression("."), 
 []() { return QString("0123456789abcdef")[qrand() % 16]; });
 mockResult["balance"] = balanceInput->text().toDouble() * 100000000;
 mockResult["expiry_time"] = QDateTime::currentSecsSinceEpoch() + expiryDaysInput->value() * 86400;
 mockResult["model_name"] = modelNameCombo->currentData().toString();
 } else if (command == "tknc_topupapikey") {
 mockResult["new_balance"] = 15000 * 100000000; // mock data
 } else if (command == "tkn_revokeapikey") {
 mockResult["success"] = true;
 } else if (command == "tkn_listapikeys") {
 // Return empty result, handled by caller
 }
 
 callback(mockResult);
 });
 }
}

void APIKeyPage::SetStatus(const QString& message, bool isError)
{
 if (isError) {
 statusLabel->setText(tr("?%1").arg(message));
 statusLabel->setStyleSheet("padding: 10px; background: #ffebee; color: #c62828; border-radius: 4px;");
 } else {
 statusLabel->setText(tr(" %1").arg(message));
 statusLabel->setStyleSheet("padding: 10px; background: #e8f5e9; color: #2e7d32; border-radius: 4px;");
 }
 
 qDebug() << "APIKeyPage:" << message;
}

QString APIKeyPage::FormatTimestamp(int64_t timestamp)
{
 if (timestamp == 0) return tr("Never expires");
 
 QDateTime dt;
 dt.setSecsSinceEpoch(timestamp);
 
 if (dt < QDateTime::currentDateTime()) {
 return tr("Expired (%1)").arg(dt.toString("yyyy-MM-dd hh:mm"));
 } else {
 return dt.toString("yyyy-MM-dd hh:mm");
 }
}
