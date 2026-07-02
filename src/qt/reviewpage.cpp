#include <span.h>
#include <serialize.h>

#include <qt/reviewpage.h>
#include <qt/guiutil.h>
#include <util/log.h>
#include <util/system.h>  // [New]GetDataDir()

#include <QMessageBox>
#include <QDateTime>
#include <QHeaderView>
#include <QFileDialog>    // [New]File dialog
#include <QTextStream>     // [New]Text stream
#include <QJsonDocument>  // [New]JSON document
#include <QJsonObject>    // [New]JSON object
#include <QJsonArray>     // [New]JSON array
#include <QFile>          // [New]File operations

ReviewPage::ReviewPage(QWidget* parent) : QWidget(parent) {
    mainLayout = new QVBoxLayout(this);
    
    pendingGroup = new QGroupBox(tr("Pending Review List"), this);
    QVBoxLayout* pendingLayout = new QVBoxLayout(pendingGroup);
    
    pendingReviewTable = new QTableWidget(this);
    pendingReviewTable->setColumnCount(4);
    pendingReviewTable->setHorizontalHeaderLabels({tr("模型名称"), tr("提交者"), tr("提交时间"), tr("状态")});
    pendingReviewTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    pendingReviewTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    
    pendingLayout->addWidget(pendingReviewTable);
    mainLayout->addWidget(pendingGroup);
    
    detailsGroup = new QGroupBox(tr("Review Details"), this);
    QVBoxLayout* detailsLayout = new QVBoxLayout(detailsGroup);
    
    modelNameLabel = new QLabel(tr("Model Name: -"), this);
    detailsLayout->addWidget(modelNameLabel);
    
    submitterLabel = new QLabel(tr("Submitter: -"), this);
    detailsLayout->addWidget(submitterLabel);
    
    submitTimeLabel = new QLabel(tr("Submit Time: -"), this);
    detailsLayout->addWidget(submitTimeLabel);
    
    modelDescription = new QTextEdit(this);
    modelDescription->setReadOnly(true);
    detailsLayout->addWidget(modelDescription);
    
    mainLayout->addWidget(detailsGroup);
    
    actionGroup = new QGroupBox(tr("Review Actions"), this);
    QVBoxLayout* actionLayout = new QVBoxLayout(actionGroup);
    
    reviewComment = new QTextEdit(this);
    reviewComment->setPlaceholderText(tr("Enter review comment..."));
    actionLayout->addWidget(reviewComment);
    
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    approveButton = new QPushButton(tr("Approved"), this);
    rejectButton = new QPushButton(tr("Rejected"), this);
    buttonLayout->addWidget(approveButton);
    buttonLayout->addWidget(rejectButton);
    
    // [New]ExportButton
    exportCSVButton = new QPushButton(tr("Export CSV"), this);
    exportJSONButton = new QPushButton(tr("Export JSON"), this);
    buttonLayout->addWidget(exportCSVButton);
    buttonLayout->addWidget(exportJSONButton);
    
    actionLayout->addLayout(buttonLayout);
    
    mainLayout->addWidget(actionGroup);
    
    statsGroup = new QGroupBox(tr("Review Statistics"), this);
    QVBoxLayout* statsLayout = new QVBoxLayout(statsGroup);
    
    totalReviewsLabel = new QLabel(tr("Total Reviews: 0"), this);
    statsLayout->addWidget(totalReviewsLabel);
    
    approvedLabel = new QLabel(tr("Approved: 0"), this);
    statsLayout->addWidget(approvedLabel);
    
    rejectedLabel = new QLabel(tr("Rejected: 0"), this);
    statsLayout->addWidget(rejectedLabel);
    
    reviewProgress = new QProgressBar(this);
    reviewProgress->setRange(0, 100);
    reviewProgress->setValue(0);
    statsLayout->addWidget(reviewProgress);
    
    mainLayout->addWidget(statsGroup);
    
    connect(pendingReviewTable, &QTableWidget::cellClicked, this, &ReviewPage::ShowReviewDetails);
    connect(approveButton, &QPushButton::clicked, this, &ReviewPage::ApproveModel);
    connect(rejectButton, &QPushButton::clicked, this, &ReviewPage::RejectModel);
    
    // [New]Connect export button signals
    connect(exportCSVButton, &QPushButton::clicked, this, &ReviewPage::ExportToCSV);
    connect(exportJSONButton, &QPushButton::clicked, this, &ReviewPage::ExportToJSON);
    
    LoadPendingReviews();
    UpdateReviewStats();
}

void ReviewPage::LoadPendingReviews() {
    pendingReviewTable->setRowCount(0);
    
    // [Modified]Load pending reviews from real data source
    // Priority: RPC call > Local cache > Empty list hint
    
    // TODO: Replace the following code after integrating backend RPC
    // Example: clientModel->node().executeRpc("tknc_listpendingreviews", ...)
    
    // [Temporary]Check for local pending review data file
    QString dataDir = GetDataDir().string().c_str();
    QString reviewFile = dataDir + "/pending_reviews.json";
    
    if (QFile::exists(reviewFile)) {
        // Load from local JSON file
        QFile file(reviewFile);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QByteArray jsonData = file.readAll();
            file.close();
            
            QJsonDocument doc = QJsonDocument::fromJson(jsonData);
            if (doc.isArray()) {
                QJsonArray reviews = doc.array();
                
                for (const QJsonValue& value : reviews) {
                    QJsonObject review = value.toObject();
                    
                    int row = pendingReviewTable->rowCount();
                    pendingReviewTable->insertRow(row);
                    pendingReviewTable->setItem(row, 0, new QTableWidgetItem(review["model_name"].toString()));
                    pendingReviewTable->setItem(row, 1, new QTableWidgetItem(review["submitter"].toString()));
                    pendingReviewTable->setItem(row, 2, new QTableWidgetItem(
                        QDateTime::fromSecsSinceEpoch(review["submit_time"].toInt()).toString("yyyy-MM-dd hh:mm:ss")));
                    pendingReviewTable->setItem(row, 3, new QTableWidgetItem(tr("Pending Review")));
                }
                
                qDebug() << "ReviewPage: Loaded from local file" << reviews.size() << "pending review records";
                return;
            }
        }
    }
    
    // [Default]No pending review data, show empty list hint
    if (pendingReviewTable->rowCount() == 0) {
        qDebug() << "ReviewPage: No pending reviews";
        
        // Do not show fake data, only update stats to 0
        UpdateReviewStats();
    }
}

void ReviewPage::ShowReviewDetails(int row) {
    if (row < 0 || row >= pendingReviewTable->rowCount()) {
        qWarning() << "ReviewPage: Invalid row index:" << row;
        return;
    }
    
    QString modelName = pendingReviewTable->item(row, 0)->text();
    QString submitter = pendingReviewTable->item(row, 1)->text();
    QString submitTime = pendingReviewTable->item(row, 2)->text();
    
    // [Modified]Show real review details（Get from table data）
    modelNameLabel->setText(tr("Model Name: %1").arg(modelName));
    submitterLabel->setText(tr("Submitter: %1").arg(submitter));
    submitTimeLabel->setText(tr("Submit Time: %1").arg(submitTime));
    
    // [Modified]Model description: if local model file exists, try reading README or config
    QString descriptionText;
    
    // TODO: Get full model description from RPC after backend integration
    // Current: show basic info hint
    descriptionText = tr("Model: %1\nSubmitter: %2\nSubmit Time: %3\n\n"
                         "Status: Pending Review\n\n"
                         "[Note]Full model details will be shown after backend integration。\n"
                         "Currently showing basic info only。").arg(modelName, submitter, submitTime);
    
    modelDescription->setPlainText(descriptionText);
    
    qDebug() << "ReviewPage: Show review details - Model:" << modelName << "Submitter:" << submitter;
}

void ReviewPage::ApproveModel() {
    int currentRow = pendingReviewTable->currentRow();
    
    if (currentRow < 0 || currentRow >= pendingReviewTable->rowCount()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select a pending model first"));
        return;
    }
    
    QString modelName = pendingReviewTable->item(currentRow, 0)->text();
    
    // [Modified]ConfirmReview Actions
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, 
        tr("确认Approved"),
        tr("确定要Approved模型 \"%1\" 的审核吗？").arg(modelName),
        QMessageBox::Yes | QMessageBox::No
    );
    
    if (reply == QMessageBox::Yes) {
        // UpdateTableStatus
        pendingReviewTable->setItem(currentRow, 3, new QTableWidgetItem(tr("已Approved")));
        
        // Set row color to green (approved)
        for (int col = 0; col < pendingReviewTable->columnCount(); ++col) {
            QTableWidgetItem* item = pendingReviewTable->item(currentRow, col);
            if (item) {
                item->setBackground(QColor(200, 255, 200));  // Light green background
            }
        }
        
        // TODO: Call after integrating backend RPC：tknc_approvereview(modelName)
        // Example: clientModel->node().executeRpc("tknc_approvereview", params)
        
        qDebug() << "ReviewPage: Model approved -" << modelName;
        
        // UpdateStatisticsInfo
        UpdateReviewStats();
        
        QMessageBox::information(this, tr("Approved审核"), 
            tr("模型 \"%1\" 已成功Approved审核！").arg(modelName));
    }
}

void ReviewPage::RejectModel() {
    int currentRow = pendingReviewTable->currentRow();
    
    if (currentRow < 0 || currentRow >= pendingReviewTable->rowCount()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select a pending model first"));
        return;
    }
    
    QString modelName = pendingReviewTable->item(currentRow, 0)->text();
    QString comment = reviewComment->toPlainText();
    
    // [Modified]Confirm reject action (review comment required)
    if (comment.isEmpty()) {
        QMessageBox::warning(this, tr("缺少审核意见"), 
            tr("Rejected审核时必须填写审核意见。\n请在下方文本框中输入Rejected原因。"));
        reviewComment->setFocus();
        return;
    }
    
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, 
        tr("确认Rejected"),
        tr("确定要Rejected模型 \"%1\" 的审核吗？\n\nRejectedreason: %2").arg(modelName, comment),
        QMessageBox::Yes | QMessageBox::No
    );
    
    if (reply == QMessageBox::Yes) {
        // UpdateTableStatus
        pendingReviewTable->setItem(currentRow, 3, new QTableWidgetItem(tr("已Rejected")));
        
        // Set row color to red (rejected)
        for (int col = 0; col < pendingReviewTable->columnCount(); ++col) {
            QTableWidgetItem* item = pendingReviewTable->item(currentRow, col);
            if (item) {
                item->setBackground(QColor(255, 200, 200));  // Light red background
            }
        }
        
        // TODO: Call after integrating backend RPC：tknc_rejectreview(modelName, comment)
        // Example: clientModel->node().executeRpc("tknc_rejectreview", params)
        
        qDebug() << "ReviewPage: Model rejected -" << modelName << "reason:" << comment;
        
        // UpdateStatisticsInfo
        UpdateReviewStats();
        
        // Clear review comment
        reviewComment->clear();
        
        QMessageBox::information(this, tr("Rejected审核"), 
            tr("模型 \"%1\" 已被Rejected审核！\n\nRejectedreason: %2").arg(modelName, comment));
    }
}

void ReviewPage::UpdateReviewStats() {
    // [Modified]Calculate statistics from actual table data
    int totalReviews = pendingReviewTable->rowCount();
    int approvedCount = 0;
    int rejectedCount = 0;
    
    // Iterate table to count each status
    for (int row = 0; row < totalReviews; ++row) {
        QTableWidgetItem* statusItem = pendingReviewTable->item(row, 3);
        if (statusItem) {
            QString status = statusItem->text();
            if (status == tr("已Approved")) {
                approvedCount++;
            } else if (status == tr("已Rejected")) {
                rejectedCount++;
            }
        }
    }
    
    // UpdateStatisticsTag
    totalReviewsLabel->setText(tr("Total Reviews: %1").arg(totalReviews));
    approvedLabel->setText(tr("Approved: %1").arg(approvedCount));
    rejectedLabel->setText(tr("Rejected: %1").arg(rejectedCount));
    
    // Calculate and update progress bar (reviewed ratio)
    int reviewedCount = approvedCount + rejectedCount;
    if (totalReviews > 0) {
        int progress = (int)((double)reviewedCount / totalReviews * 100);
        reviewProgress->setValue(progress);
        
        qDebug() << "ReviewPage: Review statistics updated - total:" << totalReviews 
                 << "Approved:" << approvedCount << "Rejected:" << rejectedCount 
                 << "Progress:" << progress << "%";
    } else {
        reviewProgress->setValue(0);
    }
}

// ========== [New]Export feature implementation ==========

void ReviewPage::ExportToCSV() {
    // Check if there is data to export
    if (pendingReviewTable->rowCount() == 0) {
        QMessageBox::warning(this, tr("导出失败"), tr("没有Pending Review数据可导出"));
        return;
    }

    // Open file save dialog
    QString fileName = QFileDialog::getSaveFileName(
        this,
        tr("导出审核报告为CSV"),
        QString("TKNC_ReviewReport_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss")),
        tr("CSV Files (*.csv);;All Files (*)")
    );

    if (fileName.isEmpty()) {
        return;  // User cancelled
    }

    // Create and write CSV file
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("导出失败"), tr("无法创建文件:\n%1").arg(fileName));
        return;
    }

    QTextStream stream(&file);
    
    // Set UTF-8 BOM (Excel compatible)
    stream.setEncoding(QStringConverter::Utf8);

    // Write CSV header
    stream << "Model Name,Submitter,Submit Time,Status\n";

    // Write data rows
    for (int row = 0; row < pendingReviewTable->rowCount(); ++row) {
        QStringList rowData;
        for (int col = 0; col < pendingReviewTable->columnCount(); ++col) {
            QTableWidgetItem* item = pendingReviewTable->item(row, col);
            if (item) {
                // CSV fields containing commas or quotes need escaping
                QString text = item->text();
                if (text.contains(',') || text.contains('"') || text.contains('\n')) {
                    text = '"' + text.replace('"', "\"\"") + '"';
                }
                rowData.append(text);
            } else {
                rowData.append("");
            }
        }
        stream << rowData.join(",") << "\n";
    }

    file.close();

    qDebug() << "ReviewPage: CSV export success - file:" << fileName 
             << "Record Count:" << pendingReviewTable->rowCount();
    
    QMessageBox::information(
        this, 
        tr("导出成功"),
        tr("审核报告已成功导出为CSV格式！\n\n文件位置: %1\nRecord count: %2条")
           .arg(fileName)
           .arg(pendingReviewTable->rowCount())
    );
}

void ReviewPage::ExportToJSON() {
    // Check if there is data to export
    if (pendingReviewTable->rowCount() == 0) {
        QMessageBox::warning(this, tr("导出失败"), tr("没有Pending Review数据可导出"));
        return;
    }

    // Open file save dialog
    QString fileName = QFileDialog::getSaveFileName(
        this,
        tr("导出审核报告为JSON"),
        QString("TKNC_ReviewReport_%1.json").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss")),
        tr("JSON Files (*.json);;All Files (*)")
    );

    if (fileName.isEmpty()) {
        return;  // User cancelled
    }

    // Build JSON document
    QJsonDocument jsonDoc;
    QJsonObject rootObject;
    
    // Meta info
    rootObject["report_type"] = "TKNC Model Review Report";
    rootObject["generated_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    rootObject["total_records"] = pendingReviewTable->rowCount();
    rootObject["version"] = "1.0";
    
    // Data array
    QJsonArray recordsArray;
    
    for (int row = 0; row < pendingReviewTable->rowCount(); ++row) {
        QJsonObject recordObject;
        
        // Use table headers as key names
        QStringList headers;
        for (int col = 0; col < pendingReviewTable->columnCount(); ++col) {
            headers.append(pendingReviewTable->horizontalHeaderItem(col)->text());
        }
        
        // Populate data
        for (int col = 0; col < pendingReviewTable->columnCount(); ++col) {
            QTableWidgetItem* item = pendingReviewTable->item(row, col);
            if (item && col < headers.size()) {
                recordObject[headers[col]] = item->text();
            }
        }
        
        recordsArray.append(recordObject);
    }
    
    rootObject["records"] = recordsArray;
    jsonDoc.setObject(rootObject);

    // Write JSON file
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("导出失败"), tr("无法创建文件:\n%1").arg(fileName));
        return;
    }

    file.write(jsonDoc.toJson(QJsonDocument::Indented));  // Pretty format output
    file.close();

    qDebug() << "ReviewPage: JSON export success - file:" << fileName 
             << "Record Count:" << pendingReviewTable->rowCount()
             << "Size:" << file.size() << "bytes";
    
    QMessageBox::information(
        this, 
        tr("导出成功"),
        tr("审核报告已成功导出为JSON格式！\n\n文件位置: %1\nRecord count: %2条")
           .arg(fileName)
           .arg(pendingReviewTable->rowCount())
    );
}
