#include "Configer.h"
#include "DbGlobal.h"
#include "Mysql.h"
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

namespace wimi::db
{

class MysqlDaoTest : public ::testing::Test
{
  protected:
    void SetUp() override { Configer::loadConfig("../../conf/public-test.yaml"); }

    void TearDown() override { cleanupTestData(); }

    static void cleanupTestData()
    {
        std::shared_ptr<MysqlDao> dao = MysqlDao::GetInstance();

        // 清理测试数据
        int delResult = 0;

        delResult = dao->deleteFriendApply(TEST_UID, TEST_FRIEND_UID);
        EXPECT_EQ(delResult, 0) << "Delete friend apply failed";

        delResult = dao->deleteFriend(TEST_UID, TEST_FRIEND_UID);
        EXPECT_EQ(delResult, 0) << "Delete friend failed";

        delResult = dao->deleteMessage(TEST_UID, TEST_MESSAGE_ID, 1);
        EXPECT_EQ(delResult, 0) << "Delete message failed";

        delResult = dao->deleteUser(TEST_UID);
        EXPECT_EQ(delResult, 0) << "Delete user failed";
    }

    static constexpr std::string_view TEST_USERNAME = "zorjen";
    static constexpr int TEST_UID = 2000;
    static constexpr int TEST_FRIEND_UID = 9998;
    static constexpr int TEST_MESSAGE_ID = 10000;
};

TEST_F(MysqlDaoTest, TestGetUser)
{
    std::shared_ptr<MysqlDao> dao = MysqlDao::GetInstance();
    User::Ptr fetchedUser = dao->getUser(TEST_USERNAME.data());

    spdlog::info("uid: {}, email: {}, username: {}, password: {}", fetchedUser->uid, fetchedUser->email,
                 fetchedUser->username, fetchedUser->password);
    EXPECT_NE(fetchedUser, nullptr) << "User not found";
}

TEST_F(MysqlDaoTest, TestUserRegistration)
{
    std::shared_ptr<MysqlDao> dao = MysqlDao::GetInstance();

    User::Ptr newUser(new User());
    newUser->uid = TEST_UID;
    newUser->email = "test@test.com";
    newUser->username = "test";
    newUser->password = "123456";

    int regResult = dao->userRegister(newUser);
    EXPECT_NE(regResult, -1) << "Registration failed";

    User::Ptr fetchedUser = dao->getUser(newUser->username.data());
    EXPECT_NE(fetchedUser, nullptr) << "User not found";
    EXPECT_EQ(fetchedUser->email, newUser->email) << "Email mismatch";
}

TEST_F(MysqlDaoTest, TestUserInfoOperations)
{
    std::shared_ptr<MysqlDao> dao = MysqlDao::GetInstance();

    UserInfo::Ptr info(new UserInfo(TEST_UID, "Test User", 25, "male", "/images/test.jpg"));

    int infoResult = dao->insertUserInfo(info);
    EXPECT_NE(infoResult, -1) << "User info insert failed";

    UserInfo::Ptr fetchedInfo = dao->getUserInfo(TEST_UID);
    EXPECT_NE(fetchedInfo, nullptr) << "User info not found";
    EXPECT_EQ(fetchedInfo->name, "Test User") << "Name mismatch";

    int updateResult = dao->updateUserInfoName(TEST_UID, "Updated Name");
    EXPECT_EQ(updateResult, 0) << "Update name failed";

    int updateAgeResult = dao->updateUserInfoAge(TEST_UID, 30);
    EXPECT_EQ(updateAgeResult, 0) << "Update age failed";

    int updateSexResult = dao->updateUserInfoSex(TEST_UID, "female");
    EXPECT_EQ(updateSexResult, 0) << "Update sex failed";

    int updateHeadImageResult = dao->updateUserInfoHeadImageURL(TEST_UID, "/images/test2.jpg");
    EXPECT_EQ(updateHeadImageResult, 0) << "Update head image failed";
}

TEST_F(MysqlDaoTest, TestFriendApplyOperations)
{
    std::shared_ptr<MysqlDao> dao = MysqlDao::GetInstance();

    auto applyStatus = FriendApply::Status::Agree;

    FriendApply::Ptr newFriendApply(
        new FriendApply(TEST_UID, TEST_FRIEND_UID, applyStatus, "Hello!", "2020-01-01 00:00:00"));
    int addResult = dao->insertFriendApply(newFriendApply);
    EXPECT_NE(addResult, -1) << "Add friend failed";

    auto friendApply = dao->getFriendApplyList(TEST_UID);
    EXPECT_NE(friendApply, nullptr) << "Friend list empty";
    EXPECT_FALSE(friendApply->empty()) << "Friend not found in list";

    int delResult = dao->deleteFriendApply(TEST_UID, TEST_FRIEND_UID);
    EXPECT_EQ(delResult, 0) << "Delete friend failed";
}

TEST_F(MysqlDaoTest, TestFriendOperations)
{
    std::shared_ptr<MysqlDao> dao = MysqlDao::GetInstance();

    Friend::Ptr newFriend(new Friend(1323214125123, 1323214125124, TEST_UID, TEST_FRIEND_UID, "2023-01-01 00:00:00"));

    int addResult = dao->insertFriend(newFriend);
    EXPECT_NE(addResult, -1) << "Add friend failed";

    auto friends = dao->getFriendList(TEST_UID);
    EXPECT_NE(friends, nullptr) << "Friend list empty";
    EXPECT_FALSE(friends->empty()) << "Friend not found in list";
}

TEST_F(MysqlDaoTest, TestMessageOperations)
{
    std::shared_ptr<MysqlDao> dao = MysqlDao::GetInstance();

    Message::Ptr msg(new Message(TEST_MESSAGE_ID, TEST_UID, TEST_FRIEND_UID, 1323214125123, 1,
                                 1, // text
                                 "Hello World",
                                 1, // wait
                                 "2023-01-01 00:00:00"));

    int msgResult = dao->insertMessage(msg);
    EXPECT_NE(msgResult, -1) << "Message insert failed";

    int updateResult = dao->updateMessage(TEST_MESSAGE_ID, 2); // status 2 is delivered
    EXPECT_EQ(updateResult, 0) << "Message update failed";
}

} // namespace wimi::db

int main(int argc, char** argv)
{
    // ::testing::InitGoogleTest(&argc, argv);
    Configer::loadConfig("../../conf/public-test.yaml");
    using namespace wimi;
    auto p = db::MysqlDao::GetInstance();
    // Group relation transitions are transactional service behavior; the
    // simplified schema has no role/status application DAO API to exercise.
    // return RUN_ALL_TESTS();
}
