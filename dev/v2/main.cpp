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

class analyzer_manager final : public agent_t {
public :
  analyzer_manager( context_t ctx )
    : agent_t( ctx )
    , analyzers_disp_(
        // Нужен приватный, т.е. видимый только нашему менеджеру
        // диспетчер, на котором и будут работать агенты-анализаторы.
        disp::thread_pool::create_private_disp(
          // Указываем, в рамках какого SObjectizer Environment
          // будет работать диспетчер. Это нужно для корректного запуска
          // и останова диспетчера.
          so_environment(),
          // Просто захардкодим количество рабочих потоков для диспетчера.
          // В реальном приложении это количество может быть вычислено
          // на основании, например, thread::hardware_concurrency() или
          // взято из конфигурации.
          16 ) )
  {
    so_subscribe_self()
      .event( &analyzer_manager::on_new_check_request );
  }

private :
  disp::thread_pool::private_dispatcher_handle_t analyzers_disp_;


  void on_new_check_request( const check_request & msg ) {
    introduce_child_coop( *this,
      // Агент из новой кооперации будет автоматически привязан к приватному
      // диспетчеру с пулом рабочих потоков (при привязке будут использоваться
      // параметры по умолчанию).
      analyzers_disp_->binder( disp::thread_pool::bind_params_t() ),
      [&]( coop_t & coop ) {
        // В кооперацию будет входить всего один агент.
        coop.make_agent< email_analyzer >( msg.email_file_, msg.reply_to_ );
      } );
  }
};

int main() {
  try {
    do_imitation< analyzer_manager >( 5000 );
    return 0;
  }
  catch( const exception & x ) {
    cerr << "Oops! " << x.what() << endl;
  }

  return 2;
}

