#include "thread_pool.h"
#include <iostream>
using namespace std;

std::mutex g_mtx;

void test_task1() {
	std::lock_guard<std::mutex> guard{g_mtx};
	cout << "this is test1" << endl;
}
void test_task2() {
	std::lock_guard<std::mutex> guard{g_mtx};
	cout << "this is test2" << endl;
}
void test_task3() {
	std::lock_guard<std::mutex> guard{g_mtx};
	cout << "this is test3" << endl;
}
void test_task4() {
	std::lock_guard<std::mutex> guard{g_mtx};
	cout << "this is test4" << endl;
}
void test_task5() {
	std::lock_guard<std::mutex> guard{g_mtx};
	cout << "this is test5" << endl;
}

int main()
{	
	dyx::ThreadPool<chrono::steady_clock> tp{2};
	tp.execute(test_task1);
	tp.execute(test_task2, chrono::steady_clock::now() + chrono::seconds(1));
	auto d = tp.execute(test_task3, chrono::milliseconds(2000));
	d();

	tp.execute(test_task4, chrono::steady_clock::now(), chrono::seconds(1));
	tp.execute(test_task5, chrono::seconds(0), chrono::seconds(1), 5);
	this_thread::sleep_for(chrono::seconds(5));
	this_thread::sleep_for(chrono::seconds(5));
}

