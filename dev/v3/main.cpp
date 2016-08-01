#include <common/stuff.hpp>

#include <list>

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
  // Этот сигнал нам нужен для того, чтобы мы могли попробовать
  // запустить в работу очередной анализатор.
  struct try_create_next_analyzer : public signal_t {};
  // А этот сигнал будет информировать нас о том, что очередной
  // анализатор завершил свою работу.
  struct analyzer_finished : public signal_t {};

public :
  analyzer_manager( context_t ctx )
    : agent_t( ctx )
    , analyzers_disp_(
        disp::thread_pool::create_private_disp( so_environment(), 16 ) )
  {
    so_subscribe_self()
      .event( &analyzer_manager::on_new_check_request )
      // А в этом случае метод-обработчик не имеет параметров,
      // поэтому тип сигнала-инцидента указывается явно.
      .event< try_create_next_analyzer >( &analyzer_manager::on_create_new_analyzer )
      .event< analyzer_finished >( &analyzer_manager::on_analyzer_finished );
  }

private :
  const size_t max_parallel_analyzers_{ 16 };
  size_t active_analyzers_{ 0 };

  disp::thread_pool::private_dispatcher_handle_t analyzers_disp_;

  list< check_request > pending_requests_;

  void on_new_check_request( const check_request & msg ) {
    // Работаем по очень простой схеме: сперва сохраняем очередной
    // запрос в список ожидания, затем отсылаем себе сигнал для
    // попытки запустить очередного обработчика.
    // И создавать агента-анализатора будем уже при обработке сигнала.
    pending_requests_.push_back( msg );
    // Отсылаем сигнал сами себе.
    send< try_create_next_analyzer >( *this );
  }

  void on_create_new_analyzer() {
    // Запустить новый анализатор можно только если еще не достигнут
    // лимит на их количество.
    if( active_analyzers_ >= max_parallel_analyzers_ )
      return;

    lauch_new_analyzer();

    // Если список не пуст и возможность стартовать анализаторов
    // сохраняется, то продолжим это делать.
    if( !pending_requests_.empty()
        && active_analyzers_ < max_parallel_analyzers_ )
      send< try_create_next_analyzer >( *this );
  }

  void on_analyzer_finished() {
    // Фиксируем факт, что анализаторов стало меньше.
    --active_analyzers_;

    // Если есть, что запускать на обработку, делаем это.
    if( !pending_requests_.empty() )
      lauch_new_analyzer();
  }

  void lauch_new_analyzer() {
    introduce_child_coop( *this,
      analyzers_disp_->binder( disp::thread_pool::bind_params_t() ),
      [this]( coop_t & coop ) {
        coop.make_agent< email_analyzer >(
          pending_requests_.front().email_file_,
          pending_requests_.front().reply_to_ );

        // Нам нужно автоматически получить уведомление, когда эта кооперация
        // перестанет работать. Для чего мы назначаем специальный нотификатор,
        // задачей которого будет отсылка сигнала analyzer_finished.
        coop.add_dereg_notificator(
          // Нотификатор получает ряд параметров, но нам они сейчас не нужны.
          [this]( environment_t &, const string &, const coop_dereg_reason_t & ) {
            send< analyzer_finished >( *this );
          } );
      } );

    // Фиксируем тот факт, что анализаторов стало больше.
    ++active_analyzers_;

    // Соответствующую заявку в списке ожидания больше хранить не нужно.
    pending_requests_.pop_front();
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

