class users
{
private:
	std::string filename;
	std::map<std::string, std::string> *user_map;

	void load_usermap();

public:
	users(std::string filename);
	~users();

	void reload();

	bool find_user(std::string username, std::string & password);
};