#include "pch.h"

#include "Remotery/Remotery.h"
#include <deque>
#include <map>
#include <Windows.h>
#include <random>

using namespace std;

//std::cout << this_thread::get_id() << #NAME << endl; \

// part of assignment
#define MAKE_UPDATE_FUNC(NAME, DURATION) \
	void Update##NAME() { \
		rmt_ScopedCPUSample(NAME, 0); \
		auto start = chrono::high_resolution_clock::now(); \
		decltype(start) end; \
		do { \
			end = chrono::high_resolution_clock::now(); \
		} while (chrono::duration_cast<chrono::microseconds>(end - start).count() < DURATION); \
	} \

MAKE_UPDATE_FUNC(Input, 200) // no dependencies
MAKE_UPDATE_FUNC(Physics, 1000) // depends on Input
MAKE_UPDATE_FUNC(Collision, 1200) // depends on Physics
MAKE_UPDATE_FUNC(Animation, 600) // depends on Collision
MAKE_UPDATE_FUNC(Particles, 800) // depends on Collision
MAKE_UPDATE_FUNC(GameElements, 2400) // depends on Physics
MAKE_UPDATE_FUNC(Rendering, 2000) // depends on Animation, Particles, GameElements
MAKE_UPDATE_FUNC(Sound, 1000) // no dependencies

int intRand(const int & min, const int & max)
{
	static thread_local std::mt19937 generator;
	std::uniform_int_distribution<int> distribution(min, max);
	return distribution(generator);
}

std::mutex WaitMutex;
std::condition_variable WaitCV;

bool WokeUp = false;

class Task
{
public:
	Task(void(*kernel)(), vector<Task*> parents, int numChildren = 0) { this->Kernel = kernel; this->Parents = parents; OpenTasks = numChildren + 1; }
	void run() { (*Kernel)(); }
	bool IsExecutable() { return OpenTasks == 1; }

	atomic<int> OpenTasks;
	vector<Task*> Parents;

private:
	void(*Kernel)();
};

class Scheduler
{
public: 
	Scheduler(int threadNum);
	~Scheduler();

	
	void AddTask(Task* task);

	void WorkOnTask(Task* task);
	// steal some work from other threads
	bool ShowSolidarity();
	void GetTaskFromGlobal();
	Task* GetTaskFromLocal();
	
	atomic<int> NumTasks;
	atomic<int> QueueSize;
	atomic<bool> ItIsWorkshift = true;
	std::vector<std::thread> ThreadParty;

	std::vector<Task*> QueuedTasks;
	std::vector<Task*> ExecutableTasks;
	std::map<thread::id,std::deque<Task*>> LocalQueues;
	std::map< thread::id, std::mutex> Mutexes;
	vector<thread::id> ThreadIds;

	std::mutex GlobalQueueMutex;
	std::mutex OpenTasksMutex;
	std::mutex LocalQueueMutex;

	atomic<bool> StartedScheduler = false;

private:
	void CreateThreads(int threadNum);
	void Working();

	std::mutex WaitingMutex;
	std::condition_variable WaitingCV;
	std::condition_variable CondVar;

	bool Notified = false;
	int ThreadNum;
};

Scheduler::Scheduler(int threadNum)
{ 
	this->ThreadNum = threadNum;
	CreateThreads(threadNum); 
	ItIsWorkshift = true; 
}

Scheduler::~Scheduler()
{
	ItIsWorkshift = false;
	CondVar.notify_all();
	WaitCV.notify_all();
	for (auto &i : ThreadParty) { i.join(); }
}


void Scheduler::AddTask(Task* task)
{
	std::unique_lock<std::mutex> lock(GlobalQueueMutex);
	QueuedTasks.push_back(task);
	lock.unlock();
	Notified = true;
	CondVar.notify_one();
}

void UpdateSerial()
{
	rmt_ScopedCPUSample(UpdateSerial, 0);

	UpdateInput();
	UpdatePhysics();
	UpdateCollision();
	UpdateAnimation();
	UpdateParticles();
	UpdateGameElements();
	UpdateRendering();
	UpdateSound();
}

void Scheduler::WorkOnTask(Task* task)
{
	task->run();
	
	for (int i = 0; i < task->Parents.size(); i++)
	{
		--task->Parents[i]->OpenTasks;
	}
	delete(task);
	NumTasks--;
	if (NumTasks == 0)
	{
		unique_lock<std::mutex> waitLock(WaitMutex);
		WokeUp = true;
		WaitCV.notify_all();
	}
}

void Scheduler::GetTaskFromGlobal()
{
	Task* task = nullptr;
	std::unique_lock<std::mutex> lock{ GlobalQueueMutex };
	if (!QueuedTasks.empty())
	{
		task = QueuedTasks.back();
		QueuedTasks.pop_back();
		lock.unlock();
		std::unique_lock<std::mutex> localLock{ Mutexes[this_thread::get_id()] };
		deque<Task*> &LocalQueue = LocalQueues[this_thread::get_id()];
		LocalQueue.push_back(task);
		localLock.unlock();
	}
}

Task * Scheduler::GetTaskFromLocal()
{
	Task* task = nullptr;
	lock_guard<mutex>lock(Mutexes[this_thread::get_id()]);
	deque<Task*> &localQ = LocalQueues[this_thread::get_id()];
	if (!localQ.empty())
	{
		if (localQ.back()->IsExecutable())
		{
			task = localQ.back();
			localQ.pop_back();
		}
	}

	// nullptr if local q is empty or task is not executable
	return task;
}

bool Scheduler::ShowSolidarity()
{
	bool stoleTask = false;
	Task* task = nullptr;

	// first get random thread and its mutex
	int rand = intRand(0, this->ThreadNum - 1);
	thread::id otherThreadId = this->ThreadIds[rand];
	// map pair is created when first accessed feels really bad
	unique_lock<mutex> lockSteal(Mutexes[otherThreadId]);
	deque<Task*> &otherLocalQ = LocalQueues[otherThreadId];
	if (!otherLocalQ.empty())
	{
		task = otherLocalQ.front();
		otherLocalQ.pop_front();
		stoleTask = true;
	}
	lockSteal.unlock();

	// put task in stealing threads queue
	unique_lock<mutex> thisThreadsLock(Mutexes[this_thread::get_id()]);
	if (task) LocalQueues[this_thread::get_id()].push_back(task);

	return stoleTask;
}
void Scheduler::CreateThreads(int threadNum)
{
	std::cout << "Number of threads: " << threadNum << std::endl;
	for (int i = 0; i < threadNum; i++)
	{
		this->ThreadParty.push_back(
		thread ([this]()
		{
			// i feel terribly sorry
			while (!StartedScheduler)
			{
			}
			while (ItIsWorkshift)
			{
				Working();
			}
		}));

		this->LocalQueues.emplace( ThreadParty[i].get_id(), std::deque<Task*>());
		this->ThreadIds.push_back(ThreadParty[i].get_id());
	}

	StartedScheduler = true;
}

void Scheduler::Working()
{
	//CondVar.wait(lock, [this] { return QueuedTasks.size() > 0; }); waiting somehow disabled writing to the atomic counter, i dunno... memory reodering?

	// look for task in global queue and local q
	GetTaskFromGlobal();
	Task* task = GetTaskFromLocal();
	if (task)
	{
		WorkOnTask(task);
	}
	// nothing in local? help one of our fellow comrade threads, and if there is nothing just yield
	else if (!ShowSolidarity())
	{
		this_thread::yield();
	}
}

Scheduler* scheduler;
std::condition_variable cv;

void UpdateParallel(atomic<bool> isRunning)
{
	rmt_ScopedCPUSample(UpdateParallel, 0);

	// number of total tasks to complete in one update
	scheduler->NumTasks = 8;

	// Create the graph and tasks

	Task* UpdateRenderingTask = new Task(UpdateRendering, vector<Task*>(), 3);
	Task* UpdateGameElementsTask = new Task(UpdateGameElements, vector<Task*>{UpdateRenderingTask}, 1);
	Task* UpdateAnimationTask = new Task(UpdateAnimation, vector<Task*>{UpdateRenderingTask}, 1);
	Task* UpdateParticlesTask = new Task(UpdateParticles, vector<Task*>{UpdateRenderingTask}, 1);


	Task* UpdateCollisionTask = new Task(UpdateCollision, vector<Task*> {UpdateAnimationTask, UpdateParticlesTask}, 1);
	Task* UpdatePhysicsTask = new Task(UpdatePhysics, vector<Task*>{UpdateGameElementsTask, UpdateCollisionTask}, 1);

	Task* UpdateInputTask = new Task(UpdateInput, vector<Task*>{UpdatePhysicsTask});
	Task* UpdateSoundTask = new Task(UpdateSound, vector<Task*>());


	scheduler->AddTask(UpdateSoundTask);
	scheduler->AddTask(UpdateInputTask);
	scheduler->AddTask(UpdatePhysicsTask);
	scheduler->AddTask(UpdateCollisionTask);
	scheduler->AddTask(UpdateParticlesTask);
	scheduler->AddTask(UpdateAnimationTask);
	scheduler->AddTask(UpdateGameElementsTask);
	scheduler->AddTask(UpdateRenderingTask);


	// wait for tasks to finish
	std::unique_lock<std::mutex> lock{ WaitMutex };
	while(!WokeUp)
		WaitCV.wait(lock);

	WokeUp = false;
}

int main()
{
	Remotery* rmt;
	rmt_CreateGlobalInstance(&rmt);

	atomic<bool> isRunning = true;

	//thread serial([&isRunning]()
	//{
	//	while (isRunning)
	//		UpdateSerial();
	//});

	//scheduler = new Scheduler(std::thread::hardware_concurrency());
	scheduler = new Scheduler(4);

	thread parallel([&isRunning]()
	{
		while (isRunning)
		{
			UpdateParallel(&isRunning);
		}
	});
	cout << "Type anything to quit...\n";
	char c;
	cin >> c;
	cout << "Quitting...\n";
	isRunning = false;

	delete(scheduler);

	//serial.join();
	parallel.join();

	rmt_DestroyGlobalInstance(rmt);
}

