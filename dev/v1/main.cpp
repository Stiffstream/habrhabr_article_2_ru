#include <common/stuff.hpp>

// Агент для анализа содержимого одного email-а.
// Получает все нужные ему параметры в конструкторе,
// выполняет все свои действия в единственном методе so_evt_start.
class email_analyzer : public agent_t {
public :
  email_analyzer( context_t ctx,
    // Имя файла с email для анализа.
    string email_file,
    // Куда нужно отослать результат анализа.
    mbox_t reply_to )
    : agent_t(ctx), email_file_(move(email_file)), reply_to_(move(reply_to))
  {}

  virtual void so_evt_start() override {
    try {
      // Стадии обработки обозначаем лишь схематично.
      auto raw_data = load_email_from_file( email_file_ );
      auto parsed_data = parse_email( raw_data );
      auto status = check_headers( parsed_data->headers() );
      if( check_status::safe == status )
        status = check_body( parsed_data->body() );
      if( check_status::safe == status )
        status = check_attachments( parsed_data->attachments() );
      send< check_result >( reply_to_, email_file_, status );
    }
    catch( const exception & ) {
      // В случае какой-либо ошибки отсылаем статус о невозможности
      // проверки файла с email-ом по техническим причинам.
      send< check_result >(
          reply_to_, email_file_, check_status::check_failure );
    }
    // Больше мы не нужны, поэтому дерегистрируем кооперацию,
    // в которой находимся.
    so_deregister_agent_coop_normally();
  }

private :
  const string email_file_;
  const mbox_t reply_to_;
};

// Агент, который будет играть роль менеджера агентов email_analyzer.
class analyzer_manager final : public agent_t {
public :
  analyzer_manager( context_t ctx ) : agent_t( ctx )
  {
    // Класс объявлен как final, поэтому подписки агента можно сделать
    // прямо в конструкторе. Если бы final не было, подписки лучше было
    // бы вынести в метод so_define_agent(), что упростило бы разработку
    // производных классов.
    so_subscribe_self()
      // В этом случае тип сообщения, на который идет подписка,
      // выводится автоматически.
      .event( &analyzer_manager::on_new_check_request );
  }

private :
  void on_new_check_request( const check_request & msg ) {
    // Создаем кооперацию с единственным агентом внутри.
    // Эта кооперация будет дочерней для кооперации с агентом-менеджером.
    // Т.е. SObjectizer Environment проконтролирует, чтобы кооперация с
    // агентом-анализатором завершила свою работу перед тем, как
    // завершит свою работу кооперация с агентом-менеджером.
    introduce_child_coop( *this, [&]( coop_t & coop ) {
        // В кооперацию будет входить всего один агент.
        coop.make_agent< email_analyzer >( msg.email_file_, msg.reply_to_ );
      } );
  }
};

int main() {
  try {
    // Запускаем SObjectizer Environment и сразу же указываем,
    // какие действия должны быть выполнены при старте.
    wrapped_env_t sobj( []( environment_t & env ) {
      // Сначала отдельной кооперацией запускаем агента-менеджера.
      mbox_t checker_mbox;
      env.introduce_coop( [&checker_mbox]( coop_t & coop ) {
        auto manager = coop.make_agent< analyzer_manager >();
        // mbox агента-менеджера потребуется для формирования потока запросов.
        checker_mbox = manager->so_direct_mbox();
      } );

      // Следующей кооперацией будет кооперация с агентом-имитатором запросов.
      // Запускаем имитатор запроса на собственной рабочей нити, дабы обработка
      // его сообщений выполнялась независимо от обработки сообщений
      // агента-менеджера.
      env.introduce_coop(
        disp::one_thread::create_private_disp( env )->binder(),
        [checker_mbox]( coop_t & coop ) {
          coop.make_agent< requests_initiator >( checker_mbox );
        } );
    } );

    // SObjectizer запущен и работает на своих рабочих потоках.
    // Даем некоторое время на выполнение его работы и завершает приложение.
    this_thread::sleep_for( 1min );
  }
  catch( const exception & x ) {
    cerr << "Oops! " << x.what() << endl;
  }

  return 2;
}

