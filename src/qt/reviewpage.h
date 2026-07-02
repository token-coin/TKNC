#ifndef TKN_QT_REVIEWPAGE_H
#define TKN_QT_REVIEWPAGE_H

#include <tknc-build-config.h>
#include <QObject>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTextEdit>
#include <QProgressBar>

class ReviewPage : public QWidget {
    Q_OBJECT

private:
    QTableWidget* pendingReviewTable;
    
    QLabel* modelNameLabel;
    QLabel* submitterLabel;
    QLabel* submitTimeLabel;
    QTextEdit* modelDescription;
    
    QPushButton* approveButton;
    QPushButton* rejectButton;
    QTextEdit* reviewComment;
    
    QPushButton* exportCSVButton;
    QPushButton* exportJSONButton;
    
    QLabel* totalReviewsLabel;
    QLabel* approvedLabel;
    QLabel* rejectedLabel;
    QProgressBar* reviewProgress;
    
    QVBoxLayout* mainLayout;
    QGroupBox* pendingGroup;
    QGroupBox* detailsGroup;
    QGroupBox* actionGroup;
    QGroupBox* statsGroup;

public:
    explicit ReviewPage(QWidget* parent = nullptr);

private Q_SLOTS:
    void LoadPendingReviews();
    
    void ShowReviewDetails(int row);
    
    void ApproveModel();
    
    void RejectModel();
    
    void UpdateReviewStats();
    
    void ExportToCSV();
    void ExportToJSON();
};

#endif // TKN_QT_REVIEWPAGE_H
